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

module lucene_core:lucene90_doc_values_producer.impl;

import :lucene90_doc_values_producer;

import :binary_doc_values;
import :bytes_ref;
import :codec_util;
import :data_input;
import :direct_monotonic_reader;
import :direct_reader;
import :directory;
import :doc_id_set_iterator;
import :doc_values;
import :doc_values_iterator;
import :doc_values_skipper;
import :error;
import :field_infos;
import :index_input;
import :indexed_disi;
import :lucene90_doc_values_format;
import :lz4;
import :numeric_doc_values;
import :random_access_input;
import :segment_info;
import :segment_read_state;
import :sorted_doc_values;
import :sorted_numeric_doc_values;
import :sorted_set_doc_values;
import :types;

import std;

namespace lucene {

namespace {

constexpr int64_t LONG_BYTES = 8;
constexpr int64_t INT_BYTES = 4;
/// Padding the term dictionary adds so LZ4 can copy eight bytes at a time.
constexpr size_t LZ4_DECOMPRESSOR_PADDING = 7;
constexpr int32_t TERMS_BLOCK_SHIFT = Lucene90DocValuesFormat::TERMS_DICT_BLOCK_LZ4_SHIFT;
constexpr int64_t TERMS_BLOCK_MASK = Lucene90DocValuesFormat::TERMS_DICT_BLOCK_LZ4_MASK;

/// Bytes to skip when a skipper rejects an interval at `level`.
///
/// Port of `SKIP_INDEX_JUMP_LENGTH_PER_LEVEL`
/// (`codecs/lucene90/lucene90_doc_values_format.rs`): an interval holds one entry
/// per level, so skipping one interval means skipping the bytes of all the
/// intervals below it plus the bytes not read at that level.
constexpr std::array<int64_t, Lucene90DocValuesFormat::SKIP_INDEX_MAX_LEVEL> skip_index_jump_lengths() {
    std::array<int64_t, Lucene90DocValuesFormat::SKIP_INDEX_MAX_LEVEL> lengths{};
    // The interval minus the bytes already read (level and max doc id).
    lengths[0] = Lucene90DocValuesFormat::SKIP_INDEX_INTERVAL_BYTES - 5;
    for (size_t level = 1; level < Lucene90DocValuesFormat::SKIP_INDEX_MAX_LEVEL; ++level) {
        lengths[level] = lengths[level - 1];
        lengths[level] += (int64_t{1} << (level * Lucene90DocValuesFormat::SKIP_INDEX_LEVEL_SHIFT)) *
                          Lucene90DocValuesFormat::SKIP_INDEX_INTERVAL_BYTES;
        lengths[level] -= int64_t{1} << ((level - 1) * Lucene90DocValuesFormat::SKIP_INDEX_LEVEL_SHIFT);
    }
    return lengths;
}

constexpr auto SKIP_INDEX_JUMP_LENGTHS = skip_index_jump_lengths();

// ---------------------------------------------------------------------------
// Value positions
// ---------------------------------------------------------------------------

/// Maps a doc id to the index of its first value.
///
/// The reference implementation pairs every compression strategy with a dense and a sparse variant
/// (`DenseNumericDocValues*` / `SparseNumericDocValues*`, eight classes for the
/// numerics alone, and the same again for binary, sorted and sorted set).  All of
/// them differ only in how they compute the value index, so one index abstraction
/// feeds every cursor in this port.
class ValueIndex {
public:
    virtual ~ValueIndex() = default;

    [[nodiscard]] virtual Result<int32_t> next_doc() = 0;
    [[nodiscard]] virtual Result<int32_t> advance(int32_t target) = 0;
    [[nodiscard]] virtual Result<bool> advance_exact(int32_t target) = 0;
    [[nodiscard]] virtual int32_t doc_id() const = 0;
    [[nodiscard]] virtual Result<int64_t> cost() const = 0;
    /// Index of the first value of the current document.
    [[nodiscard]] virtual Result<int64_t> value_index() const = 0;
};

/// Every document has a value, so the value index is the doc id.
class DenseValueIndex final : public ValueIndex {
public:
    explicit DenseValueIndex(int32_t max_doc) : max_doc_(max_doc) {}

    [[nodiscard]] Result<int32_t> next_doc() override { return advance(doc_ + 1); }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        doc_ = target >= max_doc_ ? NO_MORE_DOCS : target;
        return doc_;
    }

    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        doc_ = target;
        return true;
    }

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(max_doc_); }
    [[nodiscard]] Result<int64_t> value_index() const override { return static_cast<int64_t>(doc_); }

private:
    int32_t max_doc_;
    int32_t doc_{-1};
};

/// Only the documents of an [`IndexedDISI`] have a value.
class SparseValueIndex final : public ValueIndex {
public:
    explicit SparseValueIndex(std::unique_ptr<IndexedDISI> doc_ids) : doc_ids_(std::move(doc_ids)) {}

    [[nodiscard]] Result<int32_t> next_doc() override { return doc_ids_->next_doc(); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override { return doc_ids_->advance(target); }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override { return doc_ids_->advance_exact(target); }
    [[nodiscard]] int32_t doc_id() const override { return doc_ids_->doc_id(); }
    [[nodiscard]] Result<int64_t> cost() const override { return doc_ids_->cost(); }

    [[nodiscard]] Result<int64_t> value_index() const override {
        return static_cast<int64_t>(doc_ids_->index());
    }

private:
    std::unique_ptr<IndexedDISI> doc_ids_;
};

/// No document has a value.
class EmptyValueIndex final : public ValueIndex {
public:
    [[nodiscard]] Result<int32_t> next_doc() override { return NO_MORE_DOCS; }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        static_cast<void>(target);
        return NO_MORE_DOCS;
    }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        static_cast<void>(target);
        return false;
    }
    [[nodiscard]] int32_t doc_id() const override { return NO_MORE_DOCS; }
    [[nodiscard]] Result<int64_t> cost() const override { return int64_t{0}; }
    [[nodiscard]] Result<int64_t> value_index() const override { return int64_t{-1}; }
};

// ---------------------------------------------------------------------------
// Numeric values
// ---------------------------------------------------------------------------

/// A sequence of `int64_t` addressed by index.
class LongValues {
public:
    virtual ~LongValues() = default;
    [[nodiscard]] virtual Result<int64_t> get(int64_t index) = 0;
};

/// Every value is the same, which is how a constant field is stored.
class ConstantLongValues final : public LongValues {
public:
    explicit ConstantLongValues(int64_t value) : value_(value) {}
    [[nodiscard]] Result<int64_t> get(int64_t index) override {
        static_cast<void>(index);
        return value_;
    }

private:
    int64_t value_;
};

/// Values packed by [`DirectWriter`], optionally through a look-up table or with
/// GCD/offset compression.
class PackedLongValues final : public LongValues {
public:
    PackedLongValues(std::unique_ptr<RandomAccessInput> slice, size_t offset, int32_t bits_per_value,
                     std::vector<int64_t> table, int64_t mul, int64_t delta)
        : slice_(std::move(slice)),
          reader_(*slice_, offset, bits_per_value),
          table_(std::move(table)),
          mul_(mul),
          delta_(delta) {}

    [[nodiscard]] Result<int64_t> get(int64_t index) override {
        auto raw = reader_.get(static_cast<size_t>(index));
        if (!raw.has_value()) {
            return std::unexpected(std::move(raw.error()));
        }
        if (!table_.empty()) {
            const auto slot = static_cast<size_t>(raw.value());
            if (slot >= table_.size()) {
                return err(LuceneError::corrupt_index("a doc values look-up table index is out of range"));
            }
            return table_[slot];
        }
        // Wrapping arithmetic, like Lucene: the encoder works modulo 2^64.
        return static_cast<int64_t>(static_cast<uint64_t>(mul_) * static_cast<uint64_t>(raw.value()) +
                                    static_cast<uint64_t>(delta_));
    }

private:
    /// Declared before `reader_`, which points into it.
    std::unique_ptr<RandomAccessInput> slice_;
    DirectReader reader_;
    std::vector<int64_t> table_;
    int64_t mul_{1};
    int64_t delta_{0};
};

/// Values split into blocks of `1 << block_shift` values, each block with its own
/// bit width and offset.
///
/// Port of `VaryingBPVReader`.  Blocks are read in index order, so the reader
/// remembers where the current block ends; a jump table lets it skip blocks.
class VaryingBpvLongValues final : public LongValues {
public:
    static Result<std::unique_ptr<LongValues>> create(const NumericEntry &entry,
                                                     std::unique_ptr<RandomAccessInput> slice, IndexInput &data) {
        std::unique_ptr<RandomAccessInput> rank;
        if (entry.value_jump_table_offset != -1) {
            auto length = data.length();
            if (!length.has_value()) {
                return std::unexpected(std::move(length.error()));
            }
            const auto offset = static_cast<size_t>(entry.value_jump_table_offset);
            if (offset > length.value()) {
                return err(LuceneError::corrupt_index("a doc values jump table starts past the end of the file"));
            }
            auto slice_result = data.random_access_slice(offset, length.value() - offset);
            if (!slice_result.has_value()) {
                return std::unexpected(std::move(slice_result.error()));
            }
            rank = std::move(slice_result.value());
        }
        std::unique_ptr<LongValues> values(
                new VaryingBpvLongValues(entry, std::move(slice), std::move(rank)));
        return values;
    }

    [[nodiscard]] Result<int64_t> get(int64_t index) override {
        const int64_t block = index >> shift_;
        if (current_block_ != block) {
            if (auto loaded = load_block(block); !loaded.has_value()) {
                return std::unexpected(std::move(loaded.error()));
            }
        }
        if (block_is_constant_) {
            return delta_;
        }
        auto raw = block_reader_.get(static_cast<size_t>(index & mask_));
        if (!raw.has_value()) {
            return std::unexpected(std::move(raw.error()));
        }
        return static_cast<int64_t>(static_cast<uint64_t>(entry_.gcd) * static_cast<uint64_t>(raw.value()) +
                                    static_cast<uint64_t>(delta_));
    }

private:
    VaryingBpvLongValues(const NumericEntry &entry, std::unique_ptr<RandomAccessInput> slice,
                         std::unique_ptr<RandomAccessInput> rank)
        : entry_(entry),
          slice_(std::move(slice)),
          rank_(std::move(rank)),
          shift_(entry.block_shift),
          mask_((int64_t{1} << entry.block_shift) - 1),
          block_reader_(*slice_, 0, 1) {}

    VoidResult load_block(int64_t block) {
        int64_t offset = block_end_offset_;
        if (rank_ != nullptr && block != current_block_ + 1) {
            auto jump = rank_->read_long(static_cast<size_t>(block) * LONG_BYTES);
            if (!jump.has_value()) {
                return std::unexpected(std::move(jump.error()));
            }
            offset = jump.value() - entry_.values_offset;
            if (offset < 0) {
                return err(LuceneError::corrupt_index("a doc values block address is before the values"));
            }
        }

        auto bits = slice_->read_byte(static_cast<size_t>(offset));
        if (!bits.has_value()) {
            return std::unexpected(std::move(bits.error()));
        }
        offset += 1;
        auto delta = slice_->read_long(static_cast<size_t>(offset));
        if (!delta.has_value()) {
            return std::unexpected(std::move(delta.error()));
        }
        offset += LONG_BYTES;
        delta_ = delta.value();

        if (bits.value() == 0) {
            block_end_offset_ = offset;
            block_is_constant_ = true;
        } else {
            auto length = slice_->read_int(static_cast<size_t>(offset));
            if (!length.has_value()) {
                return std::unexpected(std::move(length.error()));
            }
            offset += INT_BYTES;
            block_end_offset_ = offset + length.value();
            block_is_constant_ = false;
            block_reader_ = DirectReader(*slice_, static_cast<size_t>(offset), static_cast<int32_t>(bits.value()));
        }
        current_block_ = block;
        return {};
    }

    NumericEntry entry_;
    /// Declared before `block_reader_`, which points into it.
    std::unique_ptr<RandomAccessInput> slice_;
    std::unique_ptr<RandomAccessInput> rank_;
    int64_t shift_{0};
    int64_t mask_{0};
    DirectReader block_reader_;
    bool block_is_constant_{false};
    int64_t current_block_{-1};
    int64_t block_end_offset_{0};
    int64_t delta_{0};
};

// ---------------------------------------------------------------------------
// Doc values cursors
// ---------------------------------------------------------------------------

class NumericDocValuesImpl final : public NumericDocValues {
public:
    NumericDocValuesImpl(std::unique_ptr<ValueIndex> index, std::unique_ptr<LongValues> values)
        : index_(std::move(index)), values_(std::move(values)) {}

    [[nodiscard]] int32_t doc_id() const override { return index_->doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override { return index_->next_doc(); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override { return index_->advance(target); }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override { return index_->advance_exact(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return index_->cost(); }

    [[nodiscard]] Result<int64_t> long_value() override {
        auto index = index_->value_index();
        if (!index.has_value()) {
            return std::unexpected(std::move(index.error()));
        }
        return values_->get(index.value());
    }

private:
    std::unique_ptr<ValueIndex> index_;
    std::unique_ptr<LongValues> values_;
};

class BinaryDocValuesImpl final : public BinaryDocValues {
public:
    /// Fixed-width values: the value of doc `i` starts at `i * length`.
    BinaryDocValuesImpl(std::unique_ptr<ValueIndex> index, std::unique_ptr<RandomAccessInput> data,
                        int32_t length)
        : index_(std::move(index)), data_(std::move(data)), length_(length) {}

    /// Variable-width values: the end addresses are monotonic numerics.
    BinaryDocValuesImpl(std::unique_ptr<ValueIndex> index, std::unique_ptr<RandomAccessInput> data,
                        const DirectMonotonicMeta &addresses_meta,
                        std::unique_ptr<RandomAccessInput> addresses_data)
        : index_(std::move(index)),
          data_(std::move(data)),
          addresses_data_(std::move(addresses_data)),
          addresses_(std::in_place, addresses_meta, *addresses_data_) {}

    [[nodiscard]] int32_t doc_id() const override { return index_->doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override { return index_->next_doc(); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override { return index_->advance(target); }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override { return index_->advance_exact(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return index_->cost(); }

    [[nodiscard]] Result<BytesRef> binary_value() override {
        auto index = index_->value_index();
        if (!index.has_value()) {
            return std::unexpected(std::move(index.error()));
        }
        int64_t start = 0;
        int64_t end = 0;
        if (length_ >= 0) {
            start = index.value() * length_;
            end = start + length_;
        } else {
            auto from = addresses_->get(static_cast<size_t>(index.value()));
            if (!from.has_value()) {
                return std::unexpected(std::move(from.error()));
            }
            auto to = addresses_->get(static_cast<size_t>(index.value() + 1));
            if (!to.has_value()) {
                return std::unexpected(std::move(to.error()));
            }
            start = from.value();
            end = to.value();
        }
        if (end < start) {
            return err(LuceneError::corrupt_index("a doc values address range is inverted"));
        }
        std::vector<uint8_t> bytes(static_cast<size_t>(end - start));
        if (!bytes.empty()) {
            auto read = data_->read_bytes(static_cast<size_t>(start), std::span<uint8_t>(bytes));
            if (!read.has_value()) {
                return std::unexpected(std::move(read.error()));
            }
        }
        return BytesRef(std::move(bytes));
    }

private:
    std::unique_ptr<ValueIndex> index_;
    std::unique_ptr<RandomAccessInput> data_;
    /// `-1` when the values have a variable length.
    int32_t length_{-1};
    std::unique_ptr<RandomAccessInput> addresses_data_;
    std::optional<DirectMonotonicReader> addresses_;
};

/// The term dictionary of a sorted or sorted set field.
///
/// Port of `TermsDict`: blocks of 64 terms, each starting with one uncompressed
/// term that also serves as the preset dictionary of the compressed suffixes.
/// Ords are read in increasing order, so the reader keeps the current block
/// decompressed in a buffer.
///
/// **Deviation:** the reverse index (`TermsDict::seek_block`) is not read, because
/// only `lookup_ord` is needed: `lookup_term` keeps the generic binary search of
/// [`SortedDocValues`], and `terms_enum` belongs to the search layer.
class TermsDict {
public:
    static Result<std::unique_ptr<TermsDict>> create(const TermsDictEntry &entry, IndexInput &data) {
        if (!entry.terms_addresses_meta.has_value()) {
            return err(LuceneError::illegal_state("a terms dictionary entry has no block addresses"));
        }
        auto block_addresses = data.random_access_slice(static_cast<size_t>(entry.terms_addresses_offset),
                                                       static_cast<size_t>(entry.terms_addresses_length));
        if (!block_addresses.has_value()) {
            return std::unexpected(std::move(block_addresses.error()));
        }
        auto bytes = data.slice("terms", static_cast<size_t>(entry.terms_data_offset),
                                static_cast<size_t>(entry.terms_data_length));
        if (!bytes.has_value()) {
            return std::unexpected(std::move(bytes.error()));
        }
        std::unique_ptr<TermsDict> terms(
                new TermsDict(entry, std::move(block_addresses.value()), std::move(bytes.value())));
        return terms;
    }

    [[nodiscard]] Result<int64_t> get_value_count() const { return entry_->terms_dict_size; }

    [[nodiscard]] Result<BytesRef> lookup_ord(int64_t ord) {
        if (ord < 0 || ord >= entry_->terms_dict_size) {
            return err(LuceneError::illegal_argument("invalid ordinal " + std::to_string(ord) + " (max " +
                                                     std::to_string(entry_->terms_dict_size) + ")"));
        }
        if (auto sought = seek_ord(ord); !sought.has_value()) {
            return std::unexpected(std::move(sought.error()));
        }
        return BytesRef(term_);
    }

private:
    TermsDict(const TermsDictEntry &entry, std::unique_ptr<RandomAccessInput> block_addresses,
              std::unique_ptr<IndexInput> bytes)
        : entry_(&entry),
          block_addresses_data_(std::move(block_addresses)),
          block_addresses_(entry.terms_addresses_meta.value(), *block_addresses_data_),
          bytes_(std::move(bytes)),
          block_buffer_(static_cast<size_t>(entry.max_block_length + entry.max_term_length +
                                            static_cast<int32_t>(LZ4_DECOMPRESSOR_PADDING)),
                        0) {}

    /// Positions the reader on `ord`, decompressing its block when needed.
    VoidResult seek_ord(int64_t ord) {
        const int64_t block_index = ord >> TERMS_BLOCK_SHIFT;
        if (ord_ < 0 || ord < ord_ || (ord_ >> TERMS_BLOCK_SHIFT) != block_index) {
            auto address = block_addresses_.get(static_cast<size_t>(block_index));
            if (!address.has_value()) {
                return std::unexpected(std::move(address.error()));
            }
            auto sought = bytes_->seek(static_cast<size_t>(address.value()));
            if (!sought.has_value()) {
                return sought;
            }
            ord_ = (block_index << TERMS_BLOCK_SHIFT) - 1;
        }
        while (ord_ < ord) {
            if (auto moved = next_term(); !moved.has_value()) {
                return moved;
            }
        }
        return {};
    }

    /// Advances to the next ord of the dictionary.
    VoidResult next_term() {
        ++ord_;
        if (ord_ >= entry_->terms_dict_size) {
            return err(LuceneError::illegal_state("the terms dictionary is exhausted"));
        }
        if ((ord_ & TERMS_BLOCK_MASK) == 0) {
            return decompress_block();
        }

        auto token = block_input_.read_byte();
        if (!token.has_value()) {
            return std::unexpected(std::move(token.error()));
        }
        int32_t prefix_length = token.value() & 0x0F;
        int32_t suffix_length = 1 + (token.value() >> 4);
        if (prefix_length == 15) {
            auto extra = block_input_.read_vint();
            if (!extra.has_value()) {
                return std::unexpected(std::move(extra.error()));
            }
            prefix_length += extra.value();
        }
        if (suffix_length == 16) {
            auto extra = block_input_.read_vint();
            if (!extra.has_value()) {
                return std::unexpected(std::move(extra.error()));
            }
            suffix_length += extra.value();
        }
        const auto prefix = static_cast<size_t>(prefix_length);
        const auto suffix = static_cast<size_t>(suffix_length);
        if (prefix > term_.size()) {
            return err(LuceneError::corrupt_index("a terms dictionary prefix does not fit the buffer"));
        }
        term_.resize(prefix + suffix);
        auto target = std::span<uint8_t>(term_).subspan(prefix, suffix);
        if (auto read = block_input_.read_bytes(target); !read.has_value()) {
            return std::unexpected(std::move(read.error()));
        }
        return {};
    }

    /// Reads the first term of the current block and decompresses its suffixes.
    VoidResult decompress_block() {
        // The first term is kept uncompressed, so it is simply read verbatim.
        auto length = bytes_->read_vint();
        if (!length.has_value()) {
            return std::unexpected(std::move(length.error()));
        }
        term_.assign(static_cast<size_t>(length.value()), 0);
        if (!term_.empty()) {
            if (auto read = bytes_->read_bytes(std::span<uint8_t>(term_)); !read.has_value()) {
                return std::unexpected(std::move(read.error()));
            }
        }

        auto offset = bytes_->get_file_pointer();
        if (!offset.has_value()) {
            return std::unexpected(std::move(offset.error()));
        }
        if (static_cast<int64_t>(offset.value()) + 1 < entry_->terms_data_length) {
            if (!current_compressed_block_start_.has_value() ||
                current_compressed_block_start_.value() != offset.value()) {
                const auto dictionary_length = term_.size();
                auto block_length = bytes_->read_vint();
                if (!block_length.has_value()) {
                    return std::unexpected(std::move(block_length.error()));
                }
                block_buffer_offset_ = dictionary_length;
                block_buffer_length_ = static_cast<size_t>(block_length.value());
                if (dictionary_length + block_buffer_length_ > block_buffer_.size()) {
                    return err(LuceneError::corrupt_index("a terms dictionary block does not fit the buffer"));
                }
                std::copy(term_.begin(), term_.end(), block_buffer_.begin());
                auto decompressed = LZ4::decompress(*bytes_, block_length.value(), block_buffer_,
                                                    static_cast<int32_t>(dictionary_length));
                if (!decompressed.has_value()) {
                    return std::unexpected(std::move(decompressed.error()));
                }
                current_compressed_block_start_ = offset.value();
                auto end = bytes_->get_file_pointer();
                if (!end.has_value()) {
                    return std::unexpected(std::move(end.error()));
                }
                current_compressed_block_end_ = end.value();
            } else {
                // The block is still in the buffer; only skip its bytes.
                if (auto sought = bytes_->seek(current_compressed_block_end_.value()); !sought.has_value()) {
                    return sought;
                }
            }
            block_input_.reset_with_range(block_buffer_, block_buffer_offset_, block_buffer_length_);
        }
        return {};
    }

    const TermsDictEntry *entry_;
    std::unique_ptr<RandomAccessInput> block_addresses_data_;
    DirectMonotonicReader block_addresses_;
    std::unique_ptr<IndexInput> bytes_;
    std::vector<uint8_t> term_;
    int64_t ord_{-1};
    std::vector<uint8_t> block_buffer_;
    ByteArrayDataInput block_input_;
    size_t block_buffer_offset_{0};
    size_t block_buffer_length_{0};
    std::optional<size_t> current_compressed_block_start_;
    std::optional<size_t> current_compressed_block_end_;
};

class SortedDocValuesImpl final : public SortedDocValues {
public:
    SortedDocValuesImpl(std::unique_ptr<ValueIndex> index, std::unique_ptr<LongValues> ords,
                        std::unique_ptr<TermsDict> terms)
        : index_(std::move(index)), ords_(std::move(ords)), terms_(std::move(terms)) {}

    [[nodiscard]] int32_t doc_id() const override { return index_->doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override { return index_->next_doc(); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override { return index_->advance(target); }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override { return index_->advance_exact(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return index_->cost(); }

    [[nodiscard]] Result<int32_t> ord_value() override {
        auto index = index_->value_index();
        if (!index.has_value()) {
            return std::unexpected(std::move(index.error()));
        }
        auto ord = ords_->get(index.value());
        if (!ord.has_value()) {
            return std::unexpected(std::move(ord.error()));
        }
        return static_cast<int32_t>(ord.value());
    }

    [[nodiscard]] Result<BytesRef> lookup_ord(int32_t ord) override { return terms_->lookup_ord(ord); }

    [[nodiscard]] Result<int32_t> get_value_count() const override {
        auto count = terms_->get_value_count();
        if (!count.has_value()) {
            return std::unexpected(std::move(count.error()));
        }
        return static_cast<int32_t>(count.value());
    }

private:
    std::unique_ptr<ValueIndex> index_;
    std::unique_ptr<LongValues> ords_;
    std::unique_ptr<TermsDict> terms_;
};

/// A single-valued sorted set seen as a sorted set.
///
/// The reference implementation returns the sorted cursor itself through
/// `Lucene90SortedSetDocValuesEnum::Singleton`; C++ needs a distinct class because
/// the two interfaces are unrelated.
class SingletonSortedSet final : public SortedSetDocValues {
public:
    explicit SingletonSortedSet(std::unique_ptr<SortedDocValues> values) : values_(std::move(values)) {}

    [[nodiscard]] int32_t doc_id() const override { return values_->doc_id(); }
    [[nodiscard]] Result<int64_t> cost() const override { return values_->cost(); }

    [[nodiscard]] Result<int32_t> next_doc() override {
        consumed_ = false;
        return values_->next_doc();
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        consumed_ = false;
        return values_->advance(target);
    }

    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        consumed_ = false;
        return values_->advance_exact(target);
    }

    [[nodiscard]] Result<int64_t> next_ord() override {
        if (consumed_) {
            return NO_MORE_ORDS;
        }
        consumed_ = true;
        auto ord = values_->ord_value();
        if (!ord.has_value()) {
            return std::unexpected(std::move(ord.error()));
        }
        return static_cast<int64_t>(ord.value());
    }

    [[nodiscard]] Result<int32_t> doc_value_count() override { return 1; }

    [[nodiscard]] Result<BytesRef> lookup_ord(int64_t ord) override {
        return values_->lookup_ord(static_cast<int32_t>(ord));
    }

    [[nodiscard]] Result<int64_t> get_value_count() const override { return values_->get_value_count(); }

private:
    std::unique_ptr<SortedDocValues> values_;
    bool consumed_{false};
};

/// The run of values of the current document.
///
/// The run is `[addresses[index], addresses[index + 1])`, where `index` is the
/// value index of the current document.  Single-valued fields store no addresses,
/// so their run is the value index itself with a length of one.
class RunValues {
public:
    RunValues(std::unique_ptr<ValueIndex> index, std::unique_ptr<LongValues> values,
              const std::optional<DirectMonotonicMeta> &addresses_meta,
              std::unique_ptr<RandomAccessInput> addresses_data, bool single_valued)
        : index_(std::move(index)),
          values_(std::move(values)),
          addresses_data_(std::move(addresses_data)),
          addresses_(addresses_meta.has_value() && addresses_data_ != nullptr
                             ? std::optional<DirectMonotonicReader>(
                                       std::in_place, addresses_meta.value(), *addresses_data_)
                             : std::nullopt),
          single_valued_(single_valued) {
        refresh();
    }

    /// Recomputes the run of the current document.
    void refresh() {
        failure_.reset();
        auto index = index_->value_index();
        if (!index.has_value()) {
            failure_ = index.error();
            return;
        }
        if (single_valued_) {
            start_ = index.value();
            count_ = 1;
            curr_ = start_;
            return;
        }
        auto from = addresses_->get(static_cast<size_t>(index.value()));
        if (!from.has_value()) {
            failure_ = from.error();
            return;
        }
        auto to = addresses_->get(static_cast<size_t>(index.value() + 1));
        if (!to.has_value()) {
            failure_ = to.error();
            return;
        }
        start_ = from.value();
        count_ = to.value() - from.value();
        curr_ = start_;
    }

    [[nodiscard]] ValueIndex &index() noexcept { return *index_; }
    [[nodiscard]] const ValueIndex &index() const noexcept { return *index_; }
    [[nodiscard]] const std::optional<LuceneError> &failure() const noexcept { return failure_; }
    [[nodiscard]] int64_t next() noexcept { return curr_++; }
    [[nodiscard]] bool exhausted() const noexcept { return curr_ >= start_ + count_; }
    [[nodiscard]] int32_t count() const noexcept { return static_cast<int32_t>(count_); }
    [[nodiscard]] LongValues &values() noexcept { return *values_; }

private:
    std::unique_ptr<ValueIndex> index_;
    std::unique_ptr<LongValues> values_;
    std::unique_ptr<RandomAccessInput> addresses_data_;
    std::optional<DirectMonotonicReader> addresses_;
    bool single_valued_{false};
    int64_t start_{0};
    int64_t curr_{0};
    int64_t count_{0};
    std::optional<LuceneError> failure_;
};

class SortedSetDocValuesImpl final : public SortedSetDocValues {
public:
    SortedSetDocValuesImpl(std::unique_ptr<ValueIndex> index, std::unique_ptr<LongValues> ords,
                          std::unique_ptr<TermsDict> terms, const std::optional<DirectMonotonicMeta> &addresses_meta,
                          std::unique_ptr<RandomAccessInput> addresses_data)
        : runs_(std::move(index), std::move(ords), addresses_meta, std::move(addresses_data), false),
          terms_(std::move(terms)) {}

    [[nodiscard]] int32_t doc_id() const override { return runs_.index().doc_id(); }
    [[nodiscard]] Result<int64_t> cost() const override { return runs_.index().cost(); }

    [[nodiscard]] Result<int32_t> next_doc() override {
        auto doc = runs_.index().next_doc();
        if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
            return doc;
        }
        runs_.refresh();
        return doc;
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        auto doc = runs_.index().advance(target);
        if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
            return doc;
        }
        runs_.refresh();
        return doc;
    }

    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        auto exact = runs_.index().advance_exact(target);
        if (!exact.has_value()) {
            return exact;
        }
        runs_.refresh();
        return exact;
    }

    [[nodiscard]] Result<int64_t> next_ord() override {
        if (runs_.exhausted()) {
            return NO_MORE_ORDS;
        }
        return runs_.values().get(runs_.next());
    }

    [[nodiscard]] Result<int32_t> doc_value_count() override { return runs_.count(); }

    [[nodiscard]] Result<BytesRef> lookup_ord(int64_t ord) override { return terms_->lookup_ord(ord); }

    [[nodiscard]] Result<int64_t> get_value_count() const override { return terms_->get_value_count(); }

private:
    RunValues runs_;
    std::unique_ptr<TermsDict> terms_;
};

class SortedNumericDocValuesImpl final : public SortedNumericDocValues {
public:
    SortedNumericDocValuesImpl(std::unique_ptr<ValueIndex> index, std::unique_ptr<LongValues> values,
                              const std::optional<DirectMonotonicMeta> &addresses_meta,
                              std::unique_ptr<RandomAccessInput> addresses_data, bool single_valued)
        : runs_(std::move(index), std::move(values), addresses_meta, std::move(addresses_data), single_valued),
          single_valued_(single_valued) {}

    [[nodiscard]] int32_t doc_id() const override { return runs_.index().doc_id(); }
    [[nodiscard]] Result<int64_t> cost() const override { return runs_.index().cost(); }

    [[nodiscard]] Result<int32_t> next_doc() override {
        auto doc = runs_.index().next_doc();
        if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
            return doc;
        }
        runs_.refresh();
        return doc;
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        auto doc = runs_.index().advance(target);
        if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
            return doc;
        }
        runs_.refresh();
        return doc;
    }

    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        auto exact = runs_.index().advance_exact(target);
        if (!exact.has_value()) {
            return exact;
        }
        runs_.refresh();
        return exact;
    }

    [[nodiscard]] Result<int64_t> next_value() override { return runs_.values().get(runs_.next()); }

    [[nodiscard]] Result<int32_t> doc_value_count() override { return runs_.count(); }

    [[nodiscard]] bool is_single_valued() const override { return single_valued_; }

    /// **Deviation:** the reference implementation wraps the underlying `NumericDocValues` here; this
    /// port reports `unsupported_operation`, because the cursor owns no producer
    /// to build a second one.  Use `doc_value_count`/`next_value` instead.
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric_doc_values() override {
        return err(LuceneError::unsupported_operation(
                "this SortedNumericDocValues does not expose a single-valued view"));
    }

private:
    RunValues runs_;
    bool single_valued_{false};
};

/// Port of `DocValuesSkipperImpl`: the intervals are stored level by level, from
/// the finest to the coarsest, and are read strictly forward.
class SkipperImpl final : public DocValuesSkipper {
public:
    SkipperImpl(std::unique_ptr<IndexInput> input, const DocValuesSkipperEntry &entry)
        : input_(std::move(input)), entry_(entry) {
        min_doc_id_.fill(-1);
        max_doc_id_.fill(-1);
    }

    [[nodiscard]] VoidResult advance(int32_t target) override {
        if (target > entry_.max_doc_id) {
            min_doc_id_.fill(NO_MORE_DOCS);
            max_doc_id_.fill(NO_MORE_DOCS);
            return {};
        }
        while (true) {
            auto levels = input_->read_byte();
            if (!levels.has_value()) {
                return std::unexpected(std::move(levels.error()));
            }
            levels_ = levels.value();
            bool valid = true;
            for (size_t i = levels_; i > 0; --i) {
                const size_t level = i - 1;
                auto max_doc = input_->read_int();
                if (!max_doc.has_value()) {
                    return std::unexpected(std::move(max_doc.error()));
                }
                max_doc_id_[level] = max_doc.value();
                if (max_doc.value() < target) {
                    auto skipped = input_->skip_bytes(SKIP_INDEX_JUMP_LENGTHS[level]);
                    if (!skipped.has_value()) {
                        return skipped;
                    }
                    valid = false;
                    break;
                }
                auto min_doc = input_->read_int();
                auto max_value = input_->read_long();
                auto min_value = input_->read_long();
                auto doc_count = input_->read_int();
                if (!min_doc.has_value()) {
                    return std::unexpected(std::move(min_doc.error()));
                }
                if (!max_value.has_value()) {
                    return std::unexpected(std::move(max_value.error()));
                }
                if (!min_value.has_value()) {
                    return std::unexpected(std::move(min_value.error()));
                }
                if (!doc_count.has_value()) {
                    return std::unexpected(std::move(doc_count.error()));
                }
                min_doc_id_[level] = min_doc.value();
                max_value_[level] = max_value.value();
                min_value_[level] = min_value.value();
                doc_count_[level] = doc_count.value();
            }
            if (valid) {
                while (levels_ < Lucene90DocValuesFormat::SKIP_INDEX_MAX_LEVEL && max_doc_id_[levels_] >= target) {
                    ++levels_;
                }
                return {};
            }
        }
    }

    [[nodiscard]] size_t num_levels() const override { return levels_; }
    [[nodiscard]] int32_t min_doc_id(size_t level) const override { return min_doc_id_[level]; }
    [[nodiscard]] int32_t max_doc_id(size_t level) const override { return max_doc_id_[level]; }
    [[nodiscard]] int64_t min_value(size_t level) const override { return min_value_[level]; }
    [[nodiscard]] int64_t max_value(size_t level) const override { return max_value_[level]; }
    [[nodiscard]] int32_t doc_count(size_t level) const override { return doc_count_[level]; }
    [[nodiscard]] int64_t global_min_value() const override { return entry_.min_value; }
    [[nodiscard]] int64_t global_max_value() const override { return entry_.max_value; }
    [[nodiscard]] int32_t global_doc_count() const override { return entry_.doc_count; }

private:
    std::unique_ptr<IndexInput> input_;
    DocValuesSkipperEntry entry_;
    size_t levels_{1};
    std::array<int32_t, Lucene90DocValuesFormat::SKIP_INDEX_MAX_LEVEL> min_doc_id_{};
    std::array<int32_t, Lucene90DocValuesFormat::SKIP_INDEX_MAX_LEVEL> max_doc_id_{};
    std::array<int64_t, Lucene90DocValuesFormat::SKIP_INDEX_MAX_LEVEL> min_value_{};
    std::array<int64_t, Lucene90DocValuesFormat::SKIP_INDEX_MAX_LEVEL> max_value_{};
    std::array<int32_t, Lucene90DocValuesFormat::SKIP_INDEX_MAX_LEVEL> doc_count_{};
};

// ---------------------------------------------------------------------------
// Metadata parsing
// ---------------------------------------------------------------------------

VoidResult read_numeric(DataInput &meta, NumericEntry &entry) {
    auto docs_with_field_offset = meta.read_long();
    auto docs_with_field_length = meta.read_long();
    auto jump_table_entry_count = meta.read_short();
    auto dense_rank_power = meta.read_byte();
    auto num_values = meta.read_long();
    if (!docs_with_field_offset.has_value() || !docs_with_field_length.has_value() ||
        !jump_table_entry_count.has_value() || !dense_rank_power.has_value() || !num_values.has_value()) {
        return err(LuceneError::corrupt_index("a numeric doc values entry is truncated"));
    }
    entry.docs_with_field_offset = docs_with_field_offset.value();
    entry.docs_with_field_length = docs_with_field_length.value();
    entry.jump_table_entry_count = jump_table_entry_count.value();
    entry.dense_rank_power = static_cast<int8_t>(dense_rank_power.value());
    entry.num_values = num_values.value();

    auto table_size = meta.read_int();
    if (!table_size.has_value()) {
        return std::unexpected(std::move(table_size.error()));
    }
    if (table_size.value() > 256) {
        return err(LuceneError::corrupt_index("invalid doc values table size: " +
                                              std::to_string(table_size.value())));
    }
    if (table_size.value() >= 0) {
        entry.table.reserve(static_cast<size_t>(table_size.value()));
        for (int32_t i = 0; i < table_size.value(); ++i) {
            auto value = meta.read_long();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            entry.table.push_back(value.value());
        }
    }
    entry.block_shift = table_size.value() < -1 ? -2 - table_size.value() : -1;

    auto bits_per_value = meta.read_byte();
    auto min_value = meta.read_long();
    auto gcd = meta.read_long();
    auto values_offset = meta.read_long();
    auto values_length = meta.read_long();
    auto value_jump_table_offset = meta.read_long();
    if (!bits_per_value.has_value() || !min_value.has_value() || !gcd.has_value() || !values_offset.has_value() ||
        !values_length.has_value() || !value_jump_table_offset.has_value()) {
        return err(LuceneError::corrupt_index("a numeric doc values entry is truncated"));
    }
    entry.bits_per_value = static_cast<int8_t>(bits_per_value.value());
    entry.min_value = min_value.value();
    entry.gcd = gcd.value();
    entry.values_offset = values_offset.value();
    entry.values_length = values_length.value();
    entry.value_jump_table_offset = value_jump_table_offset.value();
    return {};
}

VoidResult read_binary(DataInput &meta, BinaryEntry &entry) {
    auto data_offset = meta.read_long();
    auto data_length = meta.read_long();
    auto docs_with_field_offset = meta.read_long();
    auto docs_with_field_length = meta.read_long();
    auto jump_table_entry_count = meta.read_short();
    auto dense_rank_power = meta.read_byte();
    auto num_docs_with_field = meta.read_int();
    auto min_length = meta.read_int();
    auto max_length = meta.read_int();
    if (!data_offset.has_value() || !data_length.has_value() || !docs_with_field_offset.has_value() ||
        !docs_with_field_length.has_value() || !jump_table_entry_count.has_value() ||
        !dense_rank_power.has_value() || !num_docs_with_field.has_value() || !min_length.has_value() ||
        !max_length.has_value()) {
        return err(LuceneError::corrupt_index("a binary doc values entry is truncated"));
    }
    entry.data_offset = data_offset.value();
    entry.data_length = data_length.value();
    entry.docs_with_field_offset = docs_with_field_offset.value();
    entry.docs_with_field_length = docs_with_field_length.value();
    entry.jump_table_entry_count = jump_table_entry_count.value();
    entry.dense_rank_power = static_cast<int8_t>(dense_rank_power.value());
    entry.num_docs_with_field = num_docs_with_field.value();
    entry.min_length = min_length.value();
    entry.max_length = max_length.value();

    if (entry.min_length < entry.max_length) {
        auto addresses_offset = meta.read_long();
        if (!addresses_offset.has_value()) {
            return std::unexpected(std::move(addresses_offset.error()));
        }
        entry.addresses_offset = addresses_offset.value();
        auto block_shift = meta.read_vint();
        if (!block_shift.has_value()) {
            return std::unexpected(std::move(block_shift.error()));
        }
        auto addresses_meta = DirectMonotonicMeta::load(
                meta, static_cast<int64_t>(entry.num_docs_with_field) + 1, block_shift.value());
        if (!addresses_meta.has_value()) {
            return std::unexpected(std::move(addresses_meta.error()));
        }
        entry.addresses_meta = std::move(addresses_meta.value());
        auto addresses_length = meta.read_long();
        if (!addresses_length.has_value()) {
            return std::unexpected(std::move(addresses_length.error()));
        }
        entry.addresses_length = addresses_length.value();
    }
    return {};
}

VoidResult read_terms_dict(DataInput &meta, TermsDictEntry &entry) {
    auto size = meta.read_vlong();
    auto block_shift = meta.read_int();
    if (!size.has_value() || !block_shift.has_value()) {
        return err(LuceneError::corrupt_index("a terms dictionary entry is truncated"));
    }
    entry.terms_dict_size = size.value();

    const int64_t addresses_size = (entry.terms_dict_size + TERMS_BLOCK_MASK) >> TERMS_BLOCK_SHIFT;
    auto terms_addresses_meta = DirectMonotonicMeta::load(meta, addresses_size, block_shift.value());
    if (!terms_addresses_meta.has_value()) {
        return std::unexpected(std::move(terms_addresses_meta.error()));
    }
    entry.terms_addresses_meta = std::move(terms_addresses_meta.value());

    auto max_term_length = meta.read_int();
    auto max_block_length = meta.read_int();
    auto terms_data_offset = meta.read_long();
    auto terms_data_length = meta.read_long();
    auto terms_addresses_offset = meta.read_long();
    auto terms_addresses_length = meta.read_long();
    auto index_shift = meta.read_int();
    if (!max_term_length.has_value() || !max_block_length.has_value() || !terms_data_offset.has_value() ||
        !terms_data_length.has_value() || !terms_addresses_offset.has_value() ||
        !terms_addresses_length.has_value() || !index_shift.has_value()) {
        return err(LuceneError::corrupt_index("a terms dictionary entry is truncated"));
    }
    entry.max_term_length = max_term_length.value();
    entry.max_block_length = max_block_length.value();
    entry.terms_data_offset = terms_data_offset.value();
    entry.terms_data_length = terms_data_length.value();
    entry.terms_addresses_offset = terms_addresses_offset.value();
    entry.terms_addresses_length = terms_addresses_length.value();
    entry.terms_dict_index_shift = index_shift.value();

    const int64_t index_size =
            (entry.terms_dict_size + (int64_t{1} << entry.terms_dict_index_shift) - 1) >>
            entry.terms_dict_index_shift;
    auto terms_index_addresses_meta = DirectMonotonicMeta::load(meta, 1 + index_size, block_shift.value());
    if (!terms_index_addresses_meta.has_value()) {
        return std::unexpected(std::move(terms_index_addresses_meta.error()));
    }
    entry.terms_index_addresses_meta = std::move(terms_index_addresses_meta.value());

    auto index_offset = meta.read_long();
    auto index_length = meta.read_long();
    auto index_addresses_offset = meta.read_long();
    auto index_addresses_length = meta.read_long();
    if (!index_offset.has_value() || !index_length.has_value() || !index_addresses_offset.has_value() ||
        !index_addresses_length.has_value()) {
        return err(LuceneError::corrupt_index("a terms dictionary reverse index entry is truncated"));
    }
    entry.terms_index_offset = index_offset.value();
    entry.terms_index_length = index_length.value();
    entry.terms_index_addresses_offset = index_addresses_offset.value();
    entry.terms_index_addresses_length = index_addresses_length.value();
    return {};
}

VoidResult read_sorted_numeric(DataInput &meta, SortedNumericEntry &entry);

VoidResult read_sorted(DataInput &meta, SortedEntry &entry) {
    if (auto read = read_numeric(meta, entry.ords_entry); !read.has_value()) {
        return read;
    }
    return read_terms_dict(meta, entry.terms_dict_entry);
}

VoidResult read_sorted_numeric(DataInput &meta, SortedNumericEntry &entry) {
    if (auto read = read_numeric(meta, entry.base); !read.has_value()) {
        return read;
    }
    auto num_docs_with_field = meta.read_int();
    if (!num_docs_with_field.has_value()) {
        return std::unexpected(std::move(num_docs_with_field.error()));
    }
    entry.num_docs_with_field = num_docs_with_field.value();

    if (static_cast<int64_t>(entry.num_docs_with_field) != entry.base.num_values) {
        auto addresses_offset = meta.read_long();
        if (!addresses_offset.has_value()) {
            return std::unexpected(std::move(addresses_offset.error()));
        }
        entry.addresses_offset = addresses_offset.value();
        auto block_shift = meta.read_vint();
        if (!block_shift.has_value()) {
            return std::unexpected(std::move(block_shift.error()));
        }
        auto addresses_meta = DirectMonotonicMeta::load(
                meta, static_cast<int64_t>(entry.num_docs_with_field) + 1, block_shift.value());
        if (!addresses_meta.has_value()) {
            return std::unexpected(std::move(addresses_meta.error()));
        }
        entry.addresses_meta = std::move(addresses_meta.value());
        auto addresses_length = meta.read_long();
        if (!addresses_length.has_value()) {
            return std::unexpected(std::move(addresses_length.error()));
        }
        entry.addresses_length = addresses_length.value();
    }
    return {};
}

VoidResult read_sorted_set(DataInput &meta, SortedSetEntry &entry) {
    auto multi_valued = meta.read_byte();
    if (!multi_valued.has_value()) {
        return std::unexpected(std::move(multi_valued.error()));
    }
    if (multi_valued.value() == 0) {
        SortedEntry single;
        if (auto read = read_sorted(meta, single); !read.has_value()) {
            return read;
        }
        entry.single_value_entry = std::move(single);
        return {};
    }
    if (multi_valued.value() != 1) {
        return err(LuceneError::corrupt_index("invalid doc values multiValued flag: " +
                                              std::to_string(multi_valued.value())));
    }
    SortedNumericEntry ords;
    if (auto read = read_sorted_numeric(meta, ords); !read.has_value()) {
        return read;
    }
    TermsDictEntry terms;
    if (auto dict = read_terms_dict(meta, terms); !dict.has_value()) {
        return dict;
    }
    entry.ords_entry = std::move(ords);
    entry.terms_dict_entry = std::move(terms);
    return {};
}

/// The `.dvm` file, parsed into one entry per field.
struct ParsedMetadata {
    std::map<int32_t, NumericEntry> numerics;
    std::map<int32_t, BinaryEntry> binaries;
    std::map<int32_t, SortedEntry> sorted;
    std::map<int32_t, SortedSetEntry> sorted_sets;
    std::map<int32_t, SortedNumericEntry> sorted_numerics;
    std::map<int32_t, DocValuesSkipperEntry> skippers;
};

VoidResult read_fields(DataInput &meta, const FieldInfos &infos, ParsedMetadata &parsed) {
    while (true) {
        auto field_number = meta.read_int();
        if (!field_number.has_value()) {
            return std::unexpected(std::move(field_number.error()));
        }
        if (field_number.value() == -1) {
            return {};
        }
        const FieldInfo *info = infos.by_number(field_number.value());
        if (info == nullptr) {
            return err(LuceneError::corrupt_index("doc values field number " + std::to_string(field_number.value()) +
                                                  " is not in the field infos"));
        }
        auto type = meta.read_byte();
        if (!type.has_value()) {
            return std::unexpected(std::move(type.error()));
        }

        if (info->doc_values_skip_index != DocValuesSkipIndexType::None) {
            DocValuesSkipperEntry skipper;
            auto offset = meta.read_long();
            auto length = meta.read_long();
            auto max_value = meta.read_long();
            auto min_value = meta.read_long();
            auto doc_count = meta.read_int();
            auto max_doc_id = meta.read_int();
            if (!offset.has_value() || !length.has_value() || !max_value.has_value() || !min_value.has_value() ||
                !doc_count.has_value() || !max_doc_id.has_value()) {
                return err(LuceneError::corrupt_index("a doc values skipper entry is truncated"));
            }
            skipper.offset = offset.value();
            skipper.length = length.value();
            skipper.max_value = max_value.value();
            skipper.min_value = min_value.value();
            skipper.doc_count = doc_count.value();
            skipper.max_doc_id = max_doc_id.value();
            parsed.skippers.insert_or_assign(info->number, skipper);
        }

        switch (type.value()) {
            case Lucene90DocValuesFormat::NUMERIC: {
                NumericEntry entry;
                if (auto read = read_numeric(meta, entry); !read.has_value()) {
                    return read;
                }
                parsed.numerics.insert_or_assign(info->number, std::move(entry));
                break;
            }
            case Lucene90DocValuesFormat::BINARY: {
                BinaryEntry entry;
                if (auto read = read_binary(meta, entry); !read.has_value()) {
                    return read;
                }
                parsed.binaries.insert_or_assign(info->number, std::move(entry));
                break;
            }
            case Lucene90DocValuesFormat::SORTED: {
                SortedEntry entry;
                if (auto read = read_sorted(meta, entry); !read.has_value()) {
                    return read;
                }
                parsed.sorted.insert_or_assign(info->number, std::move(entry));
                break;
            }
            case Lucene90DocValuesFormat::SORTED_SET: {
                SortedSetEntry entry;
                if (auto read = read_sorted_set(meta, entry); !read.has_value()) {
                    return read;
                }
                parsed.sorted_sets.insert_or_assign(info->number, std::move(entry));
                break;
            }
            case Lucene90DocValuesFormat::SORTED_NUMERIC: {
                SortedNumericEntry entry;
                if (auto read = read_sorted_numeric(meta, entry); !read.has_value()) {
                    return read;
                }
                parsed.sorted_numerics.insert_or_assign(info->number, std::move(entry));
                break;
            }
            default:
                return err(LuceneError::corrupt_index("invalid doc values type: " + std::to_string(type.value())));
        }
    }
}

// ---------------------------------------------------------------------------
// Cursor factories
// ---------------------------------------------------------------------------

/// The values of a numeric entry, without the doc-id mapping.
Result<std::unique_ptr<LongValues>> make_numeric_values(const NumericEntry &entry, IndexInput &data) {
    if (entry.bits_per_value == 0) {
        return std::unique_ptr<LongValues>(new ConstantLongValues(entry.min_value));
    }
    auto slice = data.random_access_slice(static_cast<size_t>(entry.values_offset),
                                          static_cast<size_t>(entry.values_length));
    if (!slice.has_value()) {
        return std::unexpected(std::move(slice.error()));
    }
    if (entry.block_shift >= 0) {
        return VaryingBpvLongValues::create(entry, std::move(slice.value()), data);
    }
    // A zero GCD cannot happen when at least one bit per value is used.
    const int64_t mul = entry.gcd == 0 ? 1 : entry.gcd;
    std::unique_ptr<LongValues> values(new PackedLongValues(std::move(slice.value()), 0, entry.bits_per_value,
                                                            entry.table, mul, entry.min_value));
    return values;
}

/// The doc-id mapping of a field: `-2` for no document, `-1` for every document,
/// otherwise the file pointer of an [`IndexedDISI`].
Result<std::unique_ptr<ValueIndex>> make_value_index(int64_t docs_with_field_offset,
                                                    int64_t docs_with_field_length,
                                                    int32_t jump_table_entry_count, int8_t dense_rank_power,
                                                    int64_t cost, IndexInput &data, int32_t max_doc) {
    if (docs_with_field_offset == -2) {
        return std::unique_ptr<ValueIndex>(new EmptyValueIndex());
    }
    if (docs_with_field_offset == -1) {
        return std::unique_ptr<ValueIndex>(new DenseValueIndex(max_doc));
    }
    auto doc_ids = IndexedDISI::create(data, static_cast<size_t>(docs_with_field_offset),
                                      static_cast<size_t>(docs_with_field_length), jump_table_entry_count,
                                      dense_rank_power, cost);
    if (!doc_ids.has_value()) {
        return std::unexpected(std::move(doc_ids.error()));
    }
    return std::unique_ptr<ValueIndex>(new SparseValueIndex(std::move(doc_ids.value())));
}

/// Same, for a numeric entry; `cost` is the number of documents that have a value.
Result<std::unique_ptr<ValueIndex>> make_value_index(const NumericEntry &entry, IndexInput &data, int32_t max_doc,
                                                    int64_t cost) {
    return make_value_index(entry.docs_with_field_offset, entry.docs_with_field_length,
                            entry.jump_table_entry_count, entry.dense_rank_power, cost, data, max_doc);
}

/// A numeric cursor over `entry`.
Result<std::unique_ptr<NumericDocValues>> make_numeric(const NumericEntry &entry, IndexInput &data,
                                                       int32_t max_doc) {
    auto index = make_value_index(entry, data, max_doc, entry.num_values);
    if (!index.has_value()) {
        return std::unexpected(std::move(index.error()));
    }
    auto values = make_numeric_values(entry, data);
    if (!values.has_value()) {
        return std::unexpected(std::move(values.error()));
    }
    return std::unique_ptr<NumericDocValues>(
            new NumericDocValuesImpl(std::move(index.value()), std::move(values.value())));
}

/// A sorted cursor over `entry`, verifying the ordinals are not compressed.
Result<std::unique_ptr<SortedDocValues>> make_sorted(const SortedEntry &entry, IndexInput &data, int32_t max_doc) {
    const NumericEntry &ords = entry.ords_entry;
    if (ords.block_shift < 0 && ords.bits_per_value > 0) {
        if (ords.gcd != 1 || ords.min_value != 0 || !ords.table.empty()) {
            return err(LuceneError::illegal_state("ordinals must not use GCD, offset or table compression"));
        }
    }
    auto index = make_value_index(ords, data, max_doc, ords.num_values);
    if (!index.has_value()) {
        return std::unexpected(std::move(index.error()));
    }
    auto values = make_numeric_values(ords, data);
    if (!values.has_value()) {
        return std::unexpected(std::move(values.error()));
    }
    auto terms = TermsDict::create(entry.terms_dict_entry, data);
    if (!terms.has_value()) {
        return std::unexpected(std::move(terms.error()));
    }
    return std::unique_ptr<SortedDocValues>(new SortedDocValuesImpl(
            std::move(index.value()), std::move(values.value()), std::move(terms.value())));
}

} // namespace

// ---------------------------------------------------------------------------
// Lucene90DocValuesProducer
// ---------------------------------------------------------------------------

Lucene90DocValuesProducer::Lucene90DocValuesProducer(std::shared_ptr<IndexInput> data, int32_t max_doc,
                                                     int32_t version)
    : data_(std::move(data)), max_doc_(max_doc), version_(version) {}

Lucene90DocValuesProducer::~Lucene90DocValuesProducer() = default;

Result<std::unique_ptr<Lucene90DocValuesProducer>> Lucene90DocValuesProducer::create(
        const SegmentReadState &state, const SegmentInfo &segment_info, std::string_view data_codec,
        std::string_view data_extension, std::string_view meta_codec, std::string_view meta_extension) {
    if (state.field_infos == nullptr) {
        return err(LuceneError::illegal_argument("doc values need the field infos of the segment"));
    }

    ParsedMetadata parsed;
    int32_t version = -1;
    const std::string meta_name = segment_file_name(segment_info.name, state.segment_suffix, meta_extension);
    {
        auto meta = state.directory.open_input(meta_name, state.context);
        if (!meta.has_value()) {
            return std::unexpected(std::move(meta.error()));
        }
        auto header = CodecUtil::check_index_header(*meta.value(), meta_codec,
                                                    Lucene90DocValuesFormat::VERSION_START,
                                                    Lucene90DocValuesFormat::VERSION_CURRENT,
                                                    std::span<const uint8_t>(segment_info.id), state.segment_suffix);
        if (!header.has_value()) {
            static_cast<void>(meta.value()->close());
            return std::unexpected(std::move(header.error()));
        }
        version = header.value();
        auto fields = read_fields(*meta.value(), *state.field_infos, parsed);
        if (!fields.has_value()) {
            static_cast<void>(meta.value()->close());
            return std::unexpected(std::move(fields.error()));
        }
        auto footer = CodecUtil::check_footer(*meta.value());
        if (!footer.has_value()) {
            static_cast<void>(meta.value()->close());
            return std::unexpected(std::move(footer.error()));
        }
        if (auto closed = meta.value()->close(); !closed.has_value()) {
            return std::unexpected(std::move(closed.error()));
        }
    }

    const std::string data_name = segment_file_name(segment_info.name, state.segment_suffix, data_extension);
    auto data = state.directory.open_input(data_name, state.context);
    if (!data.has_value()) {
        return std::unexpected(std::move(data.error()));
    }
    auto data_header = CodecUtil::check_index_header(*data.value(), data_codec,
                                                     Lucene90DocValuesFormat::VERSION_START,
                                                     Lucene90DocValuesFormat::VERSION_CURRENT,
                                                     std::span<const uint8_t>(segment_info.id), state.segment_suffix);
    if (!data_header.has_value()) {
        static_cast<void>(data.value()->close());
        return std::unexpected(std::move(data_header.error()));
    }
    if (data_header.value() != version) {
        static_cast<void>(data.value()->close());
        return err(LuceneError::corrupt_index("doc values format versions mismatch: meta=" +
                                              std::to_string(version) + ", data=" +
                                              std::to_string(data_header.value())));
    }
    // The data file is far too big to checksum on open; only the footer
    // structure is validated here, like the reference implementation.
    auto checksum = CodecUtil::retrieve_checksum(*data.value());
    if (!checksum.has_value()) {
        static_cast<void>(data.value()->close());
        return std::unexpected(std::move(checksum.error()));
    }

    std::shared_ptr<IndexInput> shared(std::move(data.value()));
    std::unique_ptr<Lucene90DocValuesProducer> producer(
            new Lucene90DocValuesProducer(std::move(shared), segment_info.max_doc, version));
    producer->numerics_ = std::move(parsed.numerics);
    producer->binaries_ = std::move(parsed.binaries);
    producer->sorted_ = std::move(parsed.sorted);
    producer->sorted_sets_ = std::move(parsed.sorted_sets);
    producer->sorted_numerics_ = std::move(parsed.sorted_numerics);
    producer->skippers_ = std::move(parsed.skippers);
    return producer;
}

const NumericEntry *Lucene90DocValuesProducer::numeric_entry(int32_t field_num) const noexcept {
    auto found = numerics_.find(field_num);
    return found == numerics_.end() ? nullptr : &found->second;
}

const BinaryEntry *Lucene90DocValuesProducer::binary_entry(int32_t field_num) const noexcept {
    auto found = binaries_.find(field_num);
    return found == binaries_.end() ? nullptr : &found->second;
}

const SortedEntry *Lucene90DocValuesProducer::sorted_entry(int32_t field_num) const noexcept {
    auto found = sorted_.find(field_num);
    return found == sorted_.end() ? nullptr : &found->second;
}

const SortedSetEntry *Lucene90DocValuesProducer::sorted_set_entry(int32_t field_num) const noexcept {
    auto found = sorted_sets_.find(field_num);
    return found == sorted_sets_.end() ? nullptr : &found->second;
}

const SortedNumericEntry *Lucene90DocValuesProducer::sorted_numeric_entry(int32_t field_num) const noexcept {
    auto found = sorted_numerics_.find(field_num);
    return found == sorted_numerics_.end() ? nullptr : &found->second;
}

Result<std::unique_ptr<NumericDocValues>> Lucene90DocValuesProducer::get_numeric(const FieldInfo &field) const {
    const NumericEntry *entry = numeric_entry(field.number);
    if (entry == nullptr) {
        return err(LuceneError::illegal_state("missing numeric doc values entry for field " +
                                              std::to_string(field.number)));
    }
    return make_numeric(*entry, *data_, max_doc_);
}

Result<std::unique_ptr<BinaryDocValues>> Lucene90DocValuesProducer::get_binary(const FieldInfo &field) const {
    const BinaryEntry *entry = binary_entry(field.number);
    if (entry == nullptr) {
        return err(LuceneError::illegal_state("missing binary doc values entry for field " +
                                              std::to_string(field.number)));
    }
    if (entry->docs_with_field_offset == -2) {
        return std::unique_ptr<BinaryDocValues>(new EmptyBinaryDocValues());
    }

    auto bytes = data_->random_access_slice(static_cast<size_t>(entry->data_offset),
                                            static_cast<size_t>(entry->data_length));
    if (!bytes.has_value()) {
        return std::unexpected(std::move(bytes.error()));
    }
    auto index = make_value_index(entry->docs_with_field_offset, entry->docs_with_field_length,
                                  entry->jump_table_entry_count, entry->dense_rank_power,
                                  static_cast<int64_t>(entry->num_docs_with_field), *data_, max_doc_);
    if (!index.has_value()) {
        return std::unexpected(std::move(index.error()));
    }

    if (entry->min_length == entry->max_length) {
        return std::unique_ptr<BinaryDocValues>(
                new BinaryDocValuesImpl(std::move(index.value()), std::move(bytes.value()), entry->max_length));
    }
    if (!entry->addresses_meta.has_value()) {
        return err(LuceneError::illegal_state("a variable-width binary entry has no addresses"));
    }
    auto addresses = data_->random_access_slice(static_cast<size_t>(entry->addresses_offset),
                                                static_cast<size_t>(entry->addresses_length));
    if (!addresses.has_value()) {
        return std::unexpected(std::move(addresses.error()));
    }
    return std::unique_ptr<BinaryDocValues>(new BinaryDocValuesImpl(
            std::move(index.value()), std::move(bytes.value()), entry->addresses_meta.value(),
            std::move(addresses.value())));
}

Result<std::unique_ptr<SortedDocValues>> Lucene90DocValuesProducer::get_sorted(const FieldInfo &field) const {
    const SortedEntry *entry = sorted_entry(field.number);
    if (entry == nullptr) {
        return err(LuceneError::illegal_state("missing sorted doc values entry for field " +
                                              std::to_string(field.number)));
    }
    return make_sorted(*entry, *data_, max_doc_);
}

Result<std::unique_ptr<SortedNumericDocValues>> Lucene90DocValuesProducer::get_sorted_numeric(
        const FieldInfo &field) const {
    const SortedNumericEntry *entry = sorted_numeric_entry(field.number);
    if (entry == nullptr) {
        return err(LuceneError::illegal_state("missing sorted numeric doc values entry for field " +
                                              std::to_string(field.number)));
    }
    auto index = make_value_index(entry->base, *data_, max_doc_, entry->base.num_values);
    if (!index.has_value()) {
        return std::unexpected(std::move(index.error()));
    }
    auto values = make_numeric_values(entry->base, *data_);
    if (!values.has_value()) {
        return std::unexpected(std::move(values.error()));
    }

    const bool single_valued = static_cast<int64_t>(entry->num_docs_with_field) == entry->base.num_values;
    std::unique_ptr<RandomAccessInput> addresses;
    if (!single_valued) {
        if (!entry->addresses_meta.has_value()) {
            return err(LuceneError::illegal_state("a multi-valued sorted numeric entry has no addresses"));
        }
        auto slice = data_->random_access_slice(static_cast<size_t>(entry->addresses_offset),
                                                static_cast<size_t>(entry->addresses_length));
        if (!slice.has_value()) {
            return std::unexpected(std::move(slice.error()));
        }
        addresses = std::move(slice.value());
    }
    return std::unique_ptr<SortedNumericDocValues>(
            new SortedNumericDocValuesImpl(std::move(index.value()), std::move(values.value()),
                                           entry->addresses_meta, std::move(addresses), single_valued));
}

Result<std::unique_ptr<SortedSetDocValues>> Lucene90DocValuesProducer::get_sorted_set(
        const FieldInfo &field) const {
    const SortedSetEntry *entry = sorted_set_entry(field.number);
    if (entry == nullptr) {
        return err(LuceneError::illegal_state("missing sorted set doc values entry for field " +
                                              std::to_string(field.number)));
    }
    if (entry->single_value_entry.has_value()) {
        auto sorted = make_sorted(entry->single_value_entry.value(), *data_, max_doc_);
        if (!sorted.has_value()) {
            return std::unexpected(std::move(sorted.error()));
        }
        return std::unique_ptr<SortedSetDocValues>(new SingletonSortedSet(std::move(sorted.value())));
    }
    if (!entry->ords_entry.has_value() || !entry->terms_dict_entry.has_value()) {
        return err(LuceneError::illegal_state("a multi-valued sorted set entry is incomplete"));
    }
    const SortedNumericEntry &ords = entry->ords_entry.value();

    auto index = make_value_index(ords.base, *data_, max_doc_, ords.base.num_values);
    if (!index.has_value()) {
        return std::unexpected(std::move(index.error()));
    }
    auto values = make_numeric_values(ords.base, *data_);
    if (!values.has_value()) {
        return std::unexpected(std::move(values.error()));
    }
    auto terms = TermsDict::create(entry->terms_dict_entry.value(), *data_);
    if (!terms.has_value()) {
        return std::unexpected(std::move(terms.error()));
    }

    // A multi-valued field stores the start address of every document's ordinals.
    std::unique_ptr<RandomAccessInput> addresses;
    if (!ords.addresses_meta.has_value()) {
        return err(LuceneError::illegal_state("a multi-valued sorted set entry has no ordinals addresses"));
    }
    auto slice = data_->random_access_slice(static_cast<size_t>(ords.addresses_offset),
                                            static_cast<size_t>(ords.addresses_length));
    if (!slice.has_value()) {
        return std::unexpected(std::move(slice.error()));
    }
    addresses = std::move(slice.value());
    return std::unique_ptr<SortedSetDocValues>(
            new SortedSetDocValuesImpl(std::move(index.value()), std::move(values.value()),
                                       std::move(terms.value()), ords.addresses_meta, std::move(addresses)));
}

Result<std::unique_ptr<DocValuesSkipper>> Lucene90DocValuesProducer::get_skipper(const FieldInfo &field) const {
    auto found = skippers_.find(field.number);
    if (found == skippers_.end()) {
        return std::unique_ptr<DocValuesSkipper>(nullptr);
    }
    const DocValuesSkipperEntry &entry = found->second;
    auto input = data_->slice("doc values skipper", static_cast<size_t>(entry.offset),
                             static_cast<size_t>(entry.length));
    if (!input.has_value()) {
        return std::unexpected(std::move(input.error()));
    }
    return std::unique_ptr<DocValuesSkipper>(new SkipperImpl(std::move(input.value()), entry));
}

VoidResult Lucene90DocValuesProducer::check_integrity() const {
    if (data_ == nullptr) {
        return err(LuceneError::illegal_state("the doc values data file is closed"));
    }
    auto checksum = CodecUtil::checksum_entire_file(*data_);
    if (!checksum.has_value()) {
        return std::unexpected(std::move(checksum.error()));
    }
    return {};
}

VoidResult Lucene90DocValuesProducer::close() const {
    if (data_ == nullptr) {
        return {};
    }
    auto closed = data_->close();
    data_.reset();
    return closed;
}

Result<std::unique_ptr<DocValuesProducer>> Lucene90DocValuesProducer::get_merge_instance() const {
    if (data_ == nullptr) {
        return err(LuceneError::illegal_state("the doc values data file is closed"));
    }
    auto cloned = data_->clone();
    if (!cloned.has_value()) {
        return std::unexpected(std::move(cloned.error()));
    }
    std::shared_ptr<IndexInput> shared(std::move(cloned.value()));
    std::unique_ptr<Lucene90DocValuesProducer> producer(
            new Lucene90DocValuesProducer(std::move(shared), max_doc_, version_));
    producer->numerics_ = numerics_;
    producer->binaries_ = binaries_;
    producer->sorted_ = sorted_;
    producer->sorted_sets_ = sorted_sets_;
    producer->sorted_numerics_ = sorted_numerics_;
    producer->skippers_ = skippers_;
    return producer;
}

} // namespace lucene
