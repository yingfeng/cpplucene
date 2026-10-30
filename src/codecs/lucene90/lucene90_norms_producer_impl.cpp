/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

module lucene_core:lucene90_norms_producer.impl;

import :lucene90_norms_producer;

import :bit_util;
import :codec_util;
import :data_input;
import :directory;
import :doc_id_set_iterator;
import :doc_values_iterator;
import :error;
import :field_infos;
import :index_input;
import :indexed_disi;
import :io_context;
import :lucene90_norms_format;
import :norms_producer;
import :numeric_doc_values;
import :random_access_input;
import :segment_info;
import :segment_read_state;
import :types;

import std;

namespace lucene {

namespace {

/// Reads the values of one field: either one constant, or a packed array indexed
/// by the ordinal of the document among the documents that have a norm.
///
/// This replaces the reference implementation's `DenseNormsIteratorBaseEnum` / `SparseNormsIteratorBaseEnum`,
/// whose variants only differ in the width of the value they read.
class NormsValueReader {
public:
    static Result<NormsValueReader> create(const std::shared_ptr<IndexInput> &data,
                                          const Lucene90NormsProducer::NormsEntry &entry, int32_t num_docs) {
        NormsValueReader reader;
        reader.bytes_per_norm_ = entry.bytes_per_norm;
        if (entry.bytes_per_norm == 0) {
            // Every document has the same norm; the metadata holds the value.
            reader.singleton_ = entry.norms_address;
            return reader;
        }
        if (entry.bytes_per_norm != 1 && entry.bytes_per_norm != 2 && entry.bytes_per_norm != 4 &&
            entry.bytes_per_norm != 8) {
            return err(LuceneError::corrupt_index("unexpected bytes per norm: " +
                                                  std::to_string(entry.bytes_per_norm)));
        }
        if (num_docs < 0 || entry.norms_address < 0) {
            return err(LuceneError::corrupt_index("the norms metadata has a negative offset or length"));
        }
        const size_t length =
                static_cast<size_t>(num_docs) * static_cast<size_t>(entry.bytes_per_norm);
        auto slice = data->random_access_slice(static_cast<size_t>(entry.norms_address), length);
        if (!slice.has_value()) {
            return std::unexpected(std::move(slice.error()));
        }
        // A memory mapped slice hands out its bytes, which turns the per
        // document read below into a plain load instead of a virtual call plus a
        // bounds check plus a `memcpy`.
        reader.bytes_ = slice.value()->contiguous_bytes().value_or(std::span<const uint8_t>{});
        reader.slice_ = std::move(slice.value());
        return reader;
    }

    [[nodiscard]] Result<int64_t> value_at(size_t ordinal) const {
        if (bytes_per_norm_ == 0) {
            return singleton_;
        }
        if (!bytes_.empty()) {
            return value_at_mapped(ordinal);
        }
        if (slice_ == nullptr) {
            return err(LuceneError::illegal_state("the norms values were never opened"));
        }
        const size_t offset = ordinal * static_cast<size_t>(bytes_per_norm_);
        switch (bytes_per_norm_) {
            case 1: {
                auto value = slice_->read_byte(offset);
                if (!value.has_value()) {
                    return std::unexpected(std::move(value.error()));
                }
                return static_cast<int64_t>(static_cast<int8_t>(value.value()));
            }
            case 2: {
                auto value = slice_->read_short(offset);
                if (!value.has_value()) {
                    return std::unexpected(std::move(value.error()));
                }
                return static_cast<int64_t>(value.value());
            }
            case 4: {
                auto value = slice_->read_int(offset);
                if (!value.has_value()) {
                    return std::unexpected(std::move(value.error()));
                }
                return static_cast<int64_t>(value.value());
            }
            case 8: {
                return slice_->read_long(offset);
            }
            default:
                return err(LuceneError::corrupt_index("unexpected bytes per norm"));
        }
    }

private:
    /// [`value_at`] over the mapped bytes; `create` has already validated that
    /// the slice holds `num_docs * bytes_per_norm` bytes, so no accessor can run
    /// past its end for a valid ordinal.
    [[nodiscard]] int64_t value_at_mapped(size_t ordinal) const noexcept {
        const size_t offset = ordinal * static_cast<size_t>(bytes_per_norm_);
        switch (bytes_per_norm_) {
            case 1:
                return static_cast<int64_t>(static_cast<int8_t>(bytes_[offset]));
            case 2:
                return static_cast<int64_t>(BitUtil::get_i16_le(bytes_, offset));
            case 4:
                return static_cast<int64_t>(BitUtil::get_i32_le(bytes_, offset));
            default:
                return BitUtil::get_i64_le(bytes_, offset);
        }
    }

    std::shared_ptr<RandomAccessInput> slice_;
    /// The bytes of [`slice_`] when the directory maps the file.
    std::span<const uint8_t> bytes_;
    int64_t singleton_{0};
    int8_t bytes_per_norm_{0};
};

/// Norms for a field where no document has one.
class EmptyNormsValues final : public NumericDocValues {
public:
    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override { return NO_MORE_DOCS; }

    [[nodiscard]] Result<int32_t> advance(int32_t /*target*/) override { return NO_MORE_DOCS; }

    [[nodiscard]] Result<int32_t> slow_advance(int32_t target) override { return advance(target); }

    [[nodiscard]] Result<int64_t> cost() const override { return int64_t{0}; }

    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        doc_ = target;
        return false;
    }

    [[nodiscard]] Result<int64_t> long_value() override {
        return err(LuceneError::illegal_state("there is no norm for the current document"));
    }

private:
    int32_t doc_{-1};
};

/// Norms for a field where every document has one.
class DenseNormsValues final : public NumericDocValues {
public:
    DenseNormsValues(NormsValueReader reader, int32_t max_doc) : reader_(std::move(reader)), max_doc_(max_doc) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        if (doc_ + 1 >= max_doc_) {
            doc_ = NO_MORE_DOCS;
        } else {
            ++doc_;
        }
        return doc_;
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        doc_ = target < max_doc_ ? target : NO_MORE_DOCS;
        return doc_;
    }

    [[nodiscard]] Result<int32_t> slow_advance(int32_t target) override { return advance(target); }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(max_doc_); }

    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        static_cast<void>(advance(target));
        return doc_ != NO_MORE_DOCS;
    }

    [[nodiscard]] Result<int64_t> long_value() override {
        if (doc_ < 0 || doc_ >= max_doc_) {
            return err(LuceneError::illegal_state("there is no norm for the current document"));
        }
        return reader_.value_at(static_cast<size_t>(doc_));
    }

private:
    NormsValueReader reader_;
    int32_t max_doc_{0};
    int32_t doc_{-1};
};

/// Norms for a field where only some documents have one; the doc ids come from an
/// [`IndexedDISI`], and [`IndexedDISI::index`] is the ordinal into the values.
class SparseNormsValues final : public NumericDocValues {
public:
    SparseNormsValues(NormsValueReader reader, std::unique_ptr<IndexedDISI> docs_with_field)
        : reader_(std::move(reader)), docs_with_field_(std::move(docs_with_field)) {}

    [[nodiscard]] int32_t doc_id() const override { return docs_with_field_->doc_id(); }

    [[nodiscard]] Result<int32_t> next_doc() override { return docs_with_field_->next_doc(); }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override { return docs_with_field_->advance(target); }

    [[nodiscard]] Result<int32_t> slow_advance(int32_t target) override { return advance(target); }

    [[nodiscard]] Result<int64_t> cost() const override { return docs_with_field_->cost(); }

    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        return docs_with_field_->advance_exact(target);
    }

    [[nodiscard]] Result<int64_t> long_value() override {
        const int32_t ordinal = docs_with_field_->index();
        if (ordinal < 0) {
            return err(LuceneError::illegal_state("there is no norm for the current document"));
        }
        return reader_.value_at(static_cast<size_t>(ordinal));
    }

private:
    NormsValueReader reader_;
    std::unique_ptr<IndexedDISI> docs_with_field_;
};

} // namespace

Lucene90NormsProducer::Lucene90NormsProducer(std::shared_ptr<IndexInput> data,
                                             std::map<int32_t, NormsEntry> entries, int32_t max_doc)
    : data_(std::move(data)), entries_(std::move(entries)), max_doc_(max_doc) {}

Lucene90NormsProducer::~Lucene90NormsProducer() {
    static_cast<void>(close());
}

Result<std::unique_ptr<Lucene90NormsProducer>> Lucene90NormsProducer::create(const SegmentReadState &state,
                                                                             const SegmentInfo &segment_info) {
    const std::span<const uint8_t> id(segment_info.id);
    const std::string &suffix = state.segment_suffix;

    const std::string data_file = segment_file_name(segment_info.name, suffix, Lucene90NormsFormat::DATA_EXTENSION);
    auto data = state.directory.open_input(data_file, state.context);
    if (!data.has_value()) {
        return std::unexpected(std::move(data.error()));
    }
    std::shared_ptr<IndexInput> data_input = std::move(data.value());
    if (auto header = CodecUtil::check_index_header(*data_input, Lucene90NormsFormat::DATA_CODEC,
                                                    Lucene90NormsFormat::VERSION_START,
                                                    Lucene90NormsFormat::VERSION_CURRENT, id, suffix);
        !header.has_value()) {
        return std::unexpected(std::move(header.error()));
    }
    if (auto checksum = CodecUtil::retrieve_checksum(*data_input); !checksum.has_value()) {
        return std::unexpected(std::move(checksum.error()));
    }

    const std::string meta_file =
            segment_file_name(segment_info.name, suffix, Lucene90NormsFormat::METADATA_EXTENSION);
    auto meta = state.directory.open_checksum_input(meta_file);
    if (!meta.has_value()) {
        return std::unexpected(std::move(meta.error()));
    }
    IndexInput &meta_in = *meta.value();
    if (auto header = CodecUtil::check_index_header(meta_in, Lucene90NormsFormat::METADATA_CODEC,
                                                    Lucene90NormsFormat::VERSION_START,
                                                    Lucene90NormsFormat::VERSION_CURRENT, id, suffix);
        !header.has_value()) {
        return std::unexpected(std::move(header.error()));
    }

    std::map<int32_t, NormsEntry> entries;
    while (true) {
        auto field_number = meta_in.read_int();
        if (!field_number.has_value()) {
            return std::unexpected(std::move(field_number.error()));
        }
        if (field_number.value() == -1) {
            break;
        }

        auto read_long = [&meta_in](int64_t &target) -> VoidResult {
            auto value = meta_in.read_long();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            target = value.value();
            return {};
        };

        NormsEntry entry;
        if (auto read = read_long(entry.docs_with_field_offset); !read.has_value()) {
            return std::unexpected(std::move(read.error()));
        }
        if (auto read = read_long(entry.docs_with_field_length); !read.has_value()) {
            return std::unexpected(std::move(read.error()));
        }
        auto jump_table_entry_count = meta_in.read_short();
        if (!jump_table_entry_count.has_value()) {
            return std::unexpected(std::move(jump_table_entry_count.error()));
        }
        entry.jump_table_entry_count = jump_table_entry_count.value();
        auto dense_rank_power = meta_in.read_byte();
        if (!dense_rank_power.has_value()) {
            return std::unexpected(std::move(dense_rank_power.error()));
        }
        entry.dense_rank_power = static_cast<int8_t>(dense_rank_power.value());

        auto num_docs_with_field = meta_in.read_int();
        if (!num_docs_with_field.has_value()) {
            return std::unexpected(std::move(num_docs_with_field.error()));
        }
        entry.num_docs_with_field = num_docs_with_field.value();

        auto bytes_per_norm = meta_in.read_byte();
        if (!bytes_per_norm.has_value()) {
            return std::unexpected(std::move(bytes_per_norm.error()));
        }
        entry.bytes_per_norm = static_cast<int8_t>(bytes_per_norm.value());

        if (auto read = read_long(entry.norms_address); !read.has_value()) {
            return std::unexpected(std::move(read.error()));
        }

        if (entry.num_docs_with_field < 0 || entry.num_docs_with_field > segment_info.max_doc) {
            return err(LuceneError::corrupt_index("illegal number of documents with norms: " +
                                                  std::to_string(entry.num_docs_with_field)));
        }

        entries.insert_or_assign(field_number.value(), entry);
    }

    if (auto footer = CodecUtil::check_footer(meta_in); !footer.has_value()) {
        return std::unexpected(std::move(footer.error()));
    }

    std::unique_ptr<Lucene90NormsProducer> producer(
            new Lucene90NormsProducer(std::move(data_input), std::move(entries), segment_info.max_doc));
    return producer;
}

const Lucene90NormsProducer::NormsEntry *Lucene90NormsProducer::entry(int32_t field_num) const noexcept {
    auto found = entries_.find(field_num);
    return found == entries_.end() ? nullptr : &found->second;
}

Result<std::unique_ptr<NumericDocValues>> Lucene90NormsProducer::get_norms(const FieldInfo &field) const {
    const NormsEntry *entry_here = entry(field.number);
    if (entry_here == nullptr) {
        return err(LuceneError::illegal_state("the norms metadata is missing for field " + field.name));
    }
    const NormsEntry &norms = *entry_here;

    if (norms.num_docs_with_field == 0 || norms.docs_with_field_offset == -2) {
        return std::unique_ptr<NumericDocValues>(new EmptyNormsValues());
    }

    if (norms.docs_with_field_offset == -1) {
        // Every document has a norm, so the values are indexed by the doc id.
        auto reader = NormsValueReader::create(data_, norms, max_doc_);
        if (!reader.has_value()) {
            return std::unexpected(std::move(reader.error()));
        }
        return std::unique_ptr<NumericDocValues>(new DenseNormsValues(std::move(reader.value()), max_doc_));
    }

    if (norms.docs_with_field_offset < 0) {
        return err(LuceneError::corrupt_index("illegal address of the docs-with-field set"));
    }
    if (norms.docs_with_field_length < 0) {
        return err(LuceneError::corrupt_index("illegal length of the docs-with-field set"));
    }

    auto docs_with_field = IndexedDISI::create(*data_, static_cast<size_t>(norms.docs_with_field_offset),
                                               static_cast<size_t>(norms.docs_with_field_length),
                                               norms.jump_table_entry_count, norms.dense_rank_power,
                                               static_cast<int64_t>(norms.num_docs_with_field));
    if (!docs_with_field.has_value()) {
        return std::unexpected(std::move(docs_with_field.error()));
    }

    auto reader = NormsValueReader::create(data_, norms, norms.num_docs_with_field);
    if (!reader.has_value()) {
        return std::unexpected(std::move(reader.error()));
    }
    return std::unique_ptr<NumericDocValues>(
            new SparseNormsValues(std::move(reader.value()), std::move(docs_with_field.value())));
}

VoidResult Lucene90NormsProducer::check_integrity() const {
    if (auto checked = CodecUtil::checksum_entire_file(*data_); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    return {};
}

VoidResult Lucene90NormsProducer::close() const {
    if (data_ == nullptr) {
        return {};
    }
    auto closed = data_->close();
    data_.reset();
    return closed;
}

} // namespace lucene
