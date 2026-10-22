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

module lucene_core:lucene90_compressing_term_vectors_reader.impl;

import :lucene90_compressing_term_vectors_reader;

import :attribute_source;
import :block_packed_reader_iterator;
import :block_term_state;
import :bytes_ref;
import :codec_util;
import :compression;
import :data_input;
import :direct_writer;
import :directory;
import :error;
import :field_infos;
import :fields;
import :impacts_enum;
import :index_input;
import :io_context;
import :lucene90_compressing_term_vectors_format;
import :lucene90_fields_index_reader;
import :packed_stream;
import :postings_enum;
import :segment_info;
import :term_state;
import :term_vectors;
import :term_vectors_reader;
import :terms;
import :terms_enum;
import :types;

import std;

namespace lucene {

namespace {

constexpr size_t PREFETCH_CACHE_SIZE = 1 << 4;
constexpr size_t PREFETCH_CACHE_MASK = PREFETCH_CACHE_SIZE - 1;

/// Width of a field-flags value, i.e. `DirectWriter::bits_required(0x07)`.
int32_t flags_bits() {
    static const int32_t bits = [] {
        auto required = DirectWriter::bits_required(static_cast<int64_t>(TV_POSITIONS | TV_OFFSETS | TV_PAYLOADS));
        return required.has_value() ? required.value() : int32_t{4};
    }();
    return bits;
}

/// The packed per-field arrays of one field of one document.
struct TVTermData {
    std::vector<int32_t> prefix_lengths;
    std::vector<int32_t> suffix_lengths;
    std::vector<int32_t> term_freqs;
    /// Cumulative end offset of the postings of each term, plus a leading `0`.
    std::vector<size_t> position_index;
};

using IntVector = std::shared_ptr<std::vector<int32_t>>;

/// Everything the [`Fields`] of one document needs; shared with the `Terms` and
/// `Postings` objects it hands out, so they stay valid after the `Fields` is
/// gone.
struct TVFieldsData {
    std::vector<int32_t> field_nums;
    std::vector<int32_t> field_flags;
    std::vector<size_t> field_num_offs;
    std::vector<size_t> num_terms;
    std::vector<size_t> field_lengths;
    std::vector<TVTermData> term_data;
    std::vector<IntVector> positions;
    std::vector<IntVector> start_offsets;
    std::vector<IntVector> lengths;
    std::shared_ptr<std::vector<uint8_t>> payload_bytes;
    std::vector<IntVector> payload_index;
    std::shared_ptr<std::vector<uint8_t>> suffix_bytes;
    std::shared_ptr<const FieldInfos> field_infos;
};

/// Postings of one term of a term vector: a single document with `freq`
/// positions.
class TVPostingsEnum final : public PostingsEnum {
public:
    VoidResult reset(size_t freq, size_t position_index, IntVector positions, IntVector start_offsets,
                     IntVector lengths, IntVector payload_index,
                     std::shared_ptr<std::vector<uint8_t>> payload_bytes) {
        term_freq_ = freq;
        position_index_ = position_index;
        positions_ = std::move(positions);
        start_offsets_ = std::move(start_offsets);
        lengths_ = std::move(lengths);
        payload_index_ = std::move(payload_index);
        payload_bytes_ = std::move(payload_bytes);
        payload_length_ = 0;
        payload_offset_ = 0;
        doc_ = -1;
        i_.reset();
        return {};
    }

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        doc_ = doc_ == -1 ? 0 : NO_MORE_DOCS;
        return doc_;
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override { return slow_advance(target); }

    [[nodiscard]] Result<int64_t> cost() const override { return int64_t{1}; }

    [[nodiscard]] Result<int32_t> freq() override {
        if (auto checked = check_doc(); !checked.has_value()) {
            return std::unexpected(std::move(checked.error()));
        }
        return static_cast<int32_t>(term_freq_);
    }

    [[nodiscard]] Result<int32_t> next_position() override {
        if (doc_ != 0) {
            return err(LuceneError::illegal_state("DocsEnum not started"));
        }
        size_t index = 0;
        bool past_end = false;
        if (i_.has_value()) {
            index = i_.value() + 1;
            past_end = index >= term_freq_;
        } else {
            past_end = term_freq_ == 0;
        }
        i_ = index;
        if (past_end) {
            return err(LuceneError::illegal_state("Read past last position"));
        }
        if (payload_index_ != nullptr && !payload_index_->empty()) {
            const size_t base = position_index_ + index;
            payload_offset_ = static_cast<size_t>((*payload_index_)[base]);
            payload_length_ = static_cast<size_t>((*payload_index_)[base + 1]) - payload_offset_;
        }
        if (positions_ == nullptr || positions_->empty()) {
            return -1;
        }
        return (*positions_)[position_index_ + index];
    }

    [[nodiscard]] Result<int32_t> start_offset() const override {
        auto index = check_position();
        if (!index.has_value()) {
            return std::unexpected(std::move(index.error()));
        }
        if (start_offsets_ == nullptr || start_offsets_->empty()) {
            return -1;
        }
        return (*start_offsets_)[position_index_ + index.value()];
    }

    [[nodiscard]] Result<int32_t> end_offset() const override {
        auto index = check_position();
        if (!index.has_value()) {
            return std::unexpected(std::move(index.error()));
        }
        if (start_offsets_ == nullptr || start_offsets_->empty()) {
            return -1;
        }
        const size_t at = position_index_ + index.value();
        return (*start_offsets_)[at] + (*lengths_)[at];
    }

    [[nodiscard]] Result<std::optional<std::span<const uint8_t>>> get_payload() const override {
        auto index = check_position();
        if (!index.has_value()) {
            return std::unexpected(std::move(index.error()));
        }
        if (payload_index_ == nullptr || payload_index_->empty() || payload_length_ == 0 ||
            payload_bytes_ == nullptr) {
            return std::optional<std::span<const uint8_t>>{};
        }
        return std::optional<std::span<const uint8_t>>(
                std::span<const uint8_t>(*payload_bytes_).subspan(payload_offset_, payload_length_));
    }

private:
    [[nodiscard]] VoidResult check_doc() const {
        if (doc_ == NO_MORE_DOCS) {
            return err(LuceneError::illegal_state("DocsEnum exhausted"));
        }
        if (doc_ == -1) {
            return err(LuceneError::illegal_state("DocsEnum not started"));
        }
        return {};
    }

    [[nodiscard]] Result<size_t> check_position() const {
        if (auto checked = check_doc(); !checked.has_value()) {
            return std::unexpected(std::move(checked.error()));
        }
        if (!i_.has_value()) {
            return err(LuceneError::illegal_state("Position enum not started"));
        }
        if (i_.value() >= term_freq_) {
            return err(LuceneError::illegal_state("Read past last position"));
        }
        return i_.value();
    }

    IntVector positions_;
    IntVector start_offsets_;
    IntVector lengths_;
    IntVector payload_index_;
    std::shared_ptr<std::vector<uint8_t>> payload_bytes_;
    size_t position_index_{0};
    size_t term_freq_{0};
    int32_t doc_{-1};
    std::optional<size_t> i_;
    size_t payload_length_{0};
    size_t payload_offset_{0};
};

/// Reads a value of a `DirectWriter` stream held in `bytes`.
[[nodiscard]] Result<int64_t> direct_get(const std::vector<uint8_t> &bytes, int32_t bits, size_t index) {
    return direct_packed_get(std::span<const uint8_t>(bytes), bits, index);
}

/// Iterates the (prefix compressed) terms of one field of a term vector.
class TVTermsEnum final : public TermsEnum {
public:
    TVTermsEnum(size_t num_terms, TVTermData term_data, IntVector positions, IntVector start_offsets,
                IntVector lengths, IntVector payload_index, std::shared_ptr<std::vector<uint8_t>> payload_bytes,
                std::shared_ptr<std::vector<uint8_t>> term_bytes, size_t term_offset, size_t term_length)
        : num_terms_(num_terms),
          term_data_(std::move(term_data)),
          positions_(std::move(positions)),
          start_offsets_(std::move(start_offsets)),
          lengths_(std::move(lengths)),
          payload_index_(std::move(payload_index)),
          payload_bytes_(std::move(payload_bytes)),
          input_(term_bytes->begin() + static_cast<std::ptrdiff_t>(term_offset),
                 term_bytes->begin() + static_cast<std::ptrdiff_t>(term_offset + term_length)) {}

    void reset() noexcept {
        term_.set_length(0);
        position_ = 0;
        ord_.reset();
    }

    [[nodiscard]] Result<const BytesRef *> next() override {
        if (ord_.has_value() ? (*ord_ + 1 == num_terms_) : (num_terms_ == 0)) {
            return static_cast<const BytesRef *>(nullptr);
        }
        const size_t ord = ord_.has_value() ? ord_.value() + 1 : 0;
        ord_ = ord;

        const size_t prefix_length = static_cast<size_t>(term_data_.prefix_lengths[ord]);
        const size_t suffix_length = static_cast<size_t>(term_data_.suffix_lengths[ord]);
        const size_t total_length = prefix_length + suffix_length;

        if (term_.bytes().size() < total_length) {
            term_.bytes().resize(total_length, 0);
        }
        term_.set_offset(0);
        term_.set_length(total_length);

        // Only the suffix is stored: the prefix is still in the term buffer from
        // the previous call.
        if (position_ + suffix_length > input_.size()) {
            return err(LuceneError::corrupt_index("term suffix runs past the end of the term bytes"));
        }
        std::copy_n(input_.begin() + static_cast<std::ptrdiff_t>(position_), suffix_length,
                    term_.bytes().begin() + static_cast<std::ptrdiff_t>(prefix_length));
        position_ += suffix_length;
        return &term_;
    }

    [[nodiscard]] Result<AttributeSource *> attributes() override {
        return err(LuceneError::unsupported_operation("term vectors have no attributes"));
    }

    [[nodiscard]] Result<AttributeSource *> attributes_mut() override {
        return err(LuceneError::unsupported_operation("term vectors have no attributes"));
    }

    [[nodiscard]] Result<bool> seek_exact(std::span<const uint8_t> term) override {
        auto status = seek_ceil(term);
        if (!status.has_value()) {
            return std::unexpected(std::move(status.error()));
        }
        return status.value() == SeekStatus::Found;
    }

    [[nodiscard]] Result<bool> prepare_seek_exact(std::span<const uint8_t> /*term*/) override { return true; }

    [[nodiscard]] Result<bool> get_prepare_seek_exact_status(std::span<const uint8_t> target) override {
        return seek_exact(target);
    }

    [[nodiscard]] Result<SeekStatus> seek_ceil(std::span<const uint8_t> text) override {
        if (ord_.has_value() && ord_.value() < num_terms_) {
            const int32_t comparison = term_.compare_to(text);
            if (comparison == 0) {
                return SeekStatus::Found;
            }
            if (comparison > 0) {
                reset();
            }
        }

        // Linear scan: a term vector holds a handful of terms.
        for (;;) {
            auto term = next();
            if (!term.has_value()) {
                return std::unexpected(std::move(term.error()));
            }
            if (term.value() == nullptr) {
                return SeekStatus::End;
            }
            const int32_t comparison = term.value()->compare_to(text);
            if (comparison > 0) {
                return SeekStatus::NotFound;
            }
            if (comparison == 0) {
                return SeekStatus::Found;
            }
        }
    }

    [[nodiscard]] VoidResult seek_exact_with_ord(int64_t /*ord*/) override {
        return err(LuceneError::unsupported_operation("term vectors have no ordinals"));
    }

    [[nodiscard]] VoidResult seek_exact_with_state(std::span<const uint8_t> term,
                                                   const TermStateEnum & /*state*/) override {
        auto found = seek_exact(term);
        if (!found.has_value()) {
            return std::unexpected(std::move(found.error()));
        }
        if (!found.value()) {
            return err(LuceneError::illegal_state("the term does not exist"));
        }
        return {};
    }

    [[nodiscard]] Result<const BytesRef *> term() const override { return &term_; }

    [[nodiscard]] Result<int64_t> ord() const override {
        return err(LuceneError::unsupported_operation("term vectors have no ordinals"));
    }

    [[nodiscard]] Result<int32_t> doc_freq() override { return 1; }

    [[nodiscard]] Result<int64_t> total_term_freq() override {
        if (!ord_.has_value()) {
            return err(LuceneError::illegal_state("the enum is not positioned on a term"));
        }
        return term_data_.term_freqs[ord_.value()];
    }

    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>> postings_with_flags(std::unique_ptr<PostingsEnum> reuse,
                                                                           int32_t /*flags*/) override {
        if (!ord_.has_value()) {
            return err(LuceneError::illegal_state("the enum is not positioned on a term"));
        }
        std::unique_ptr<TVPostingsEnum> docs;
        if (reuse != nullptr) {
            auto *existing = dynamic_cast<TVPostingsEnum *>(reuse.get());
            if (existing == nullptr) {
                return err(LuceneError::illegal_argument("the reused PostingsEnum has another type"));
            }
            static_cast<void>(reuse.release());
            docs.reset(existing);
        } else {
            docs = std::make_unique<TVPostingsEnum>();
        }
        if (auto reset = docs->reset(static_cast<size_t>(term_data_.term_freqs[ord_.value()]),
                                     term_data_.position_index[ord_.value()], positions_, start_offsets_, lengths_,
                                     payload_index_, payload_bytes_);
            !reset.has_value()) {
            return std::unexpected(std::move(reset.error()));
        }
        return std::unique_ptr<PostingsEnum>(std::move(docs));
    }

    [[nodiscard]] Result<std::unique_ptr<ImpactsEnum>> impacts(int32_t /*flags*/) override {
        return err(LuceneError::unsupported_operation("term vectors have no impacts"));
    }

    [[nodiscard]] Result<TermStateEnum> term_state() override {
        return TermStateEnum(OrdTermState(ord_.has_value() ? static_cast<int64_t>(ord_.value()) : int64_t{-1}));
    }

private:
    size_t num_terms_{0};
    TVTermData term_data_;
    IntVector positions_;
    IntVector start_offsets_;
    IntVector lengths_;
    IntVector payload_index_;
    std::shared_ptr<std::vector<uint8_t>> payload_bytes_;
    std::vector<uint8_t> input_;
    size_t position_{0};
    std::optional<size_t> ord_;
    BytesRef term_;
};

/// The terms of one field of one term vector.
class TVTerms final : public Terms {
public:
    TVTerms(size_t num_terms, int32_t flags, TVTermData term_data, IntVector positions, IntVector start_offsets,
            IntVector lengths, IntVector payload_index, std::shared_ptr<std::vector<uint8_t>> payload_bytes,
            std::shared_ptr<std::vector<uint8_t>> term_bytes, size_t term_offset, size_t term_length)
        : num_terms_(num_terms),
          flags_(flags),
          term_data_(std::move(term_data)),
          positions_(std::move(positions)),
          start_offsets_(std::move(start_offsets)),
          lengths_(std::move(lengths)),
          payload_index_(std::move(payload_index)),
          payload_bytes_(std::move(payload_bytes)),
          term_bytes_(std::move(term_bytes)),
          term_offset_(term_offset),
          term_length_(term_length) {
        for (const int32_t freq : term_data_.term_freqs) {
            total_term_freq_ += freq;
        }
    }

    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> iterator() const override {
        return std::unique_ptr<TermsEnum>(std::make_unique<TVTermsEnum>(
                num_terms_, term_data_, positions_, start_offsets_, lengths_, payload_index_, payload_bytes_,
                term_bytes_, term_offset_, term_length_));
    }

    [[nodiscard]] Result<int64_t> size() const override { return static_cast<int64_t>(num_terms_); }

    [[nodiscard]] Result<int64_t> sum_total_term_freq() const override { return total_term_freq_; }

    [[nodiscard]] Result<int64_t> sum_doc_freq() const override { return static_cast<int64_t>(num_terms_); }

    [[nodiscard]] Result<int32_t> doc_count() const override { return 1; }

    [[nodiscard]] bool has_freqs() const override { return true; }
    [[nodiscard]] bool has_offsets() const override { return (flags_ & TV_OFFSETS) != 0; }
    [[nodiscard]] bool has_positions() const override { return (flags_ & TV_POSITIONS) != 0; }
    [[nodiscard]] bool has_payloads() const override { return (flags_ & TV_PAYLOADS) != 0; }

private:
    size_t num_terms_{0};
    int32_t flags_{0};
    int64_t total_term_freq_{0};
    TVTermData term_data_;
    IntVector positions_;
    IntVector start_offsets_;
    IntVector lengths_;
    IntVector payload_index_;
    std::shared_ptr<std::vector<uint8_t>> payload_bytes_;
    std::shared_ptr<std::vector<uint8_t>> term_bytes_;
    size_t term_offset_{0};
    size_t term_length_{0};
};

/// The field names of one term vector, in the order the fields were written.
class TVFieldsIterator final : public FieldNameIterator {
public:
    explicit TVFieldsIterator(std::shared_ptr<TVFieldsData> data) : data_(std::move(data)) {}

    [[nodiscard]] Result<const std::string *> next() override {
        if (index_ >= data_->field_num_offs.size()) {
            return static_cast<const std::string *>(nullptr);
        }
        const int32_t field_num = data_->field_nums[data_->field_num_offs[index_]];
        ++index_;
        const FieldInfo *info = data_->field_infos->by_number(field_num);
        if (info == nullptr) {
            return err(LuceneError::illegal_state("Field number " + std::to_string(field_num) +
                                                  " not found in field infos"));
        }
        return &info->name;
    }

private:
    std::shared_ptr<TVFieldsData> data_;
    size_t index_{0};
};

/// The term vectors of one document.
class TVFields final : public Fields {
public:
    /// Validates that every field number of the vector exists in `field_infos`.
    [[nodiscard]] static Result<std::unique_ptr<TVFields>> create(std::shared_ptr<TVFieldsData> data) {
        for (const size_t offset : data->field_num_offs) {
            const int32_t field_num = data->field_nums[offset];
            if (data->field_infos->by_number(field_num) == nullptr) {
                return err(LuceneError::illegal_state("Field number " + std::to_string(field_num) +
                                                      " not found in field infos"));
            }
        }
        return std::unique_ptr<TVFields>(new TVFields(std::move(data)));
    }

    [[nodiscard]] Result<std::unique_ptr<FieldNameIterator>> iterator() const override {
        return std::unique_ptr<FieldNameIterator>(std::make_unique<TVFieldsIterator>(data_));
    }

    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view field) const override {
        const FieldInfo *info = data_->field_infos->by_name(field);
        if (info == nullptr) {
            return std::unique_ptr<Terms>{};
        }

        size_t index = data_->field_num_offs.size();
        for (size_t i = 0; i < data_->field_num_offs.size(); ++i) {
            if (data_->field_nums[data_->field_num_offs[i]] == info->number) {
                index = i;
                break;
            }
        }
        if (index == data_->field_num_offs.size() || data_->num_terms[index] == 0) {
            return std::unique_ptr<Terms>{};
        }

        size_t field_offset = 0;
        for (size_t i = 0; i < index; ++i) {
            field_offset += data_->field_lengths[i];
        }

        return std::unique_ptr<Terms>(std::make_unique<TVTerms>(
                data_->num_terms[index], data_->field_flags[index], data_->term_data[index],
                data_->positions[index], data_->start_offsets[index], data_->lengths[index],
                data_->payload_index[index], data_->payload_bytes, data_->suffix_bytes, field_offset,
                data_->field_lengths[index]));
    }

    [[nodiscard]] Result<int32_t> size() const override {
        return static_cast<int32_t>(data_->field_num_offs.size());
    }

private:
    explicit TVFields(std::shared_ptr<TVFieldsData> data) : data_(std::move(data)) {}

    std::shared_ptr<TVFieldsData> data_;
};

} // namespace

// ---- construction ----

Lucene90CompressingTermVectorsReader::Lucene90CompressingTermVectorsReader(
        std::shared_ptr<const FieldInfos> field_infos, int32_t version, int32_t packed_ints_version,
        int32_t chunk_size, int32_t num_docs, std::unique_ptr<Lucene90FieldsIndexReader> index_reader,
        std::unique_ptr<IndexInput> vectors_stream, size_t max_pointer, int64_t num_chunks, int64_t num_dirty_chunks,
        int64_t num_dirty_docs, CompressionMode compression_mode)
    : version_(version),
      packed_ints_version_(packed_ints_version),
      chunk_size_(chunk_size),
      num_docs_(num_docs),
      field_infos_(std::move(field_infos)),
      index_reader_(std::move(index_reader)),
      vectors_stream_(std::move(vectors_stream)),
      max_pointer_(max_pointer),
      num_chunks_(num_chunks),
      num_dirty_chunks_(num_dirty_chunks),
      num_dirty_docs_(num_dirty_docs),
      compression_mode_(compression_mode),
      decompressor_(compression_mode.new_decompressor()),
      prefetched_block_id_cache_(PREFETCH_CACHE_SIZE, -1) {}

Lucene90CompressingTermVectorsReader::~Lucene90CompressingTermVectorsReader() {
    static_cast<void>(close());
}

Result<std::unique_ptr<Lucene90CompressingTermVectorsReader>> Lucene90CompressingTermVectorsReader::create(
        const Directory &directory, const SegmentInfo &segment_info, std::string_view segment_suffix,
        std::shared_ptr<const FieldInfos> field_infos, const IOContext &context, std::string_view format_name,
        const CompressionMode &compression_mode) {
    auto vectors_stream = directory.open_input(
            segment_file_name(segment_info.name, segment_suffix, VECTORS_EXTENSION), context);
    if (!vectors_stream.has_value()) {
        return std::unexpected(std::move(vectors_stream.error()));
    }

    auto version = CodecUtil::check_index_header(*vectors_stream.value(), format_name, VERSION_START,
                                                 VERSION_CURRENT, segment_info.id, segment_suffix);
    if (!version.has_value()) {
        static_cast<void>(vectors_stream.value()->close());
        return std::unexpected(std::move(version.error()));
    }

    auto meta_in = directory.open_input(segment_file_name(segment_info.name, segment_suffix, VECTORS_META_EXTENSION),
                                        context);
    if (!meta_in.has_value()) {
        static_cast<void>(vectors_stream.value()->close());
        return std::unexpected(std::move(meta_in.error()));
    }
    auto meta_header = CodecUtil::check_index_header(*meta_in.value(), VECTORS_META_CODEC_NAME, META_VERSION_START,
                                                     version.value(), segment_info.id, segment_suffix);
    if (!meta_header.has_value()) {
        static_cast<void>(vectors_stream.value()->close());
        static_cast<void>(meta_in.value()->close());
        return std::unexpected(std::move(meta_header.error()));
    }

    auto packed_ints_version = meta_in.value()->read_vint();
    auto chunk_size = meta_in.value()->read_vint();
    if (!packed_ints_version.has_value() || !chunk_size.has_value()) {
        static_cast<void>(vectors_stream.value()->close());
        static_cast<void>(meta_in.value()->close());
        return err(LuceneError::corrupt_index("the term vectors metadata is truncated"));
    }

    // The data file is too big to checksum on open; only the structure of its
    // footer is validated, which catches truncation.
    if (auto checksum = CodecUtil::retrieve_checksum(*vectors_stream.value()); !checksum.has_value()) {
        static_cast<void>(vectors_stream.value()->close());
        static_cast<void>(meta_in.value()->close());
        return std::unexpected(std::move(checksum.error()));
    }

    auto index_reader = Lucene90FieldsIndexReader::create(directory, segment_info.name, segment_suffix,
                                                          VECTORS_INDEX_EXTENSION, VECTORS_INDEX_CODEC_NAME,
                                                          segment_info.id, *meta_in.value(), context);
    if (!index_reader.has_value()) {
        static_cast<void>(vectors_stream.value()->close());
        static_cast<void>(meta_in.value()->close());
        return std::unexpected(std::move(index_reader.error()));
    }
    const size_t max_pointer = index_reader.value()->get_max_pointer();

    auto num_chunks = meta_in.value()->read_vlong();
    auto num_dirty_chunks = meta_in.value()->read_vlong();
    auto num_dirty_docs = meta_in.value()->read_vlong();
    if (!num_chunks.has_value() || !num_dirty_chunks.has_value() || !num_dirty_docs.has_value()) {
        static_cast<void>(vectors_stream.value()->close());
        static_cast<void>(index_reader.value()->close());
        static_cast<void>(meta_in.value()->close());
        return err(LuceneError::corrupt_index("the term vectors metadata is truncated"));
    }
    if (num_chunks.value() < num_dirty_chunks.value()) {
        static_cast<void>(vectors_stream.value()->close());
        static_cast<void>(index_reader.value()->close());
        static_cast<void>(meta_in.value()->close());
        return err(LuceneError::corrupt_index("cannot have more dirty chunks than chunks"));
    }
    if ((num_dirty_chunks.value() == 0) != (num_dirty_docs.value() == 0)) {
        static_cast<void>(vectors_stream.value()->close());
        static_cast<void>(index_reader.value()->close());
        static_cast<void>(meta_in.value()->close());
        return err(LuceneError::corrupt_index("cannot have dirty chunks without dirty docs or vice-versa"));
    }
    if (num_dirty_docs.value() < num_dirty_chunks.value()) {
        static_cast<void>(vectors_stream.value()->close());
        static_cast<void>(index_reader.value()->close());
        static_cast<void>(meta_in.value()->close());
        return err(LuceneError::corrupt_index("cannot have more dirty chunks than documents within them"));
    }

    // The metadata file is small, so its whole checksum is verified here.
    if (auto footer = CodecUtil::checksum_entire_file(*meta_in.value()); !footer.has_value()) {
        static_cast<void>(vectors_stream.value()->close());
        static_cast<void>(index_reader.value()->close());
        static_cast<void>(meta_in.value()->close());
        return std::unexpected(std::move(footer.error()));
    }
    if (auto closed = meta_in.value()->close(); !closed.has_value()) {
        static_cast<void>(vectors_stream.value()->close());
        static_cast<void>(index_reader.value()->close());
        return std::unexpected(std::move(closed.error()));
    }

    auto packed_reader = BlockPackedReaderIterator::create(packed_ints_version.value(), PACKED_BLOCK_SIZE, 0);
    if (!packed_reader.has_value()) {
        static_cast<void>(vectors_stream.value()->close());
        static_cast<void>(index_reader.value()->close());
        return std::unexpected(std::move(packed_reader.error()));
    }

    std::unique_ptr<Lucene90CompressingTermVectorsReader> reader(new Lucene90CompressingTermVectorsReader(
            std::move(field_infos), version.value(), packed_ints_version.value(), chunk_size.value(),
            segment_info.max_doc, std::move(index_reader.value()), std::move(vectors_stream.value()), max_pointer,
            num_chunks.value(), num_dirty_chunks.value(), num_dirty_docs.value(), compression_mode));
    reader->reader_ = std::make_unique<BlockPackedReaderIterator>(std::move(packed_reader.value()));
    return reader;
}

Result<std::unique_ptr<Lucene90CompressingTermVectorsReader>> Lucene90CompressingTermVectorsReader::copy(
        bool /*for_merge*/) const {
    auto vectors_stream = vectors_stream_->clone();
    if (!vectors_stream.has_value()) {
        return std::unexpected(std::move(vectors_stream.error()));
    }
    auto index_reader = index_reader_->clone_reader();
    if (!index_reader.has_value()) {
        return std::unexpected(std::move(index_reader.error()));
    }
    auto packed_reader = BlockPackedReaderIterator::create(packed_ints_version_, PACKED_BLOCK_SIZE, 0);
    if (!packed_reader.has_value()) {
        return std::unexpected(std::move(packed_reader.error()));
    }

    std::unique_ptr<Lucene90CompressingTermVectorsReader> reader(new Lucene90CompressingTermVectorsReader(
            field_infos_, version_, packed_ints_version_, chunk_size_, num_docs_, std::move(index_reader.value()),
            std::move(vectors_stream.value()), max_pointer_, num_chunks_, num_dirty_chunks_, num_dirty_docs_,
            compression_mode_));
    reader->reader_ = std::make_unique<BlockPackedReaderIterator>(std::move(packed_reader.value()));
    return reader;
}

VoidResult Lucene90CompressingTermVectorsReader::ensure_open() const {
    if (closed_) {
        return err(LuceneError::already_closed("this TermVectorsReader is closed"));
    }
    return {};
}

bool Lucene90CompressingTermVectorsReader::is_loaded(int32_t doc_id) const noexcept {
    const int32_t doc_base = block_state_.doc_base.value_or(-1);
    return doc_base <= doc_id && doc_id < doc_base + block_state_.chunk_docs;
}

// ---- TermVectorsReader ----

VoidResult Lucene90CompressingTermVectorsReader::check_integrity() const {
    if (auto checked = index_reader_->check_integrity(); !checked.has_value()) {
        return checked;
    }
    if (auto checksum = CodecUtil::checksum_entire_file(*vectors_stream_); !checksum.has_value()) {
        return std::unexpected(std::move(checksum.error()));
    }
    return {};
}

Result<std::unique_ptr<TermVectorsReader>> Lucene90CompressingTermVectorsReader::get_merge_instance() const {
    auto reader = copy(true);
    if (!reader.has_value()) {
        return std::unexpected(std::move(reader.error()));
    }
    return std::unique_ptr<TermVectorsReader>(std::move(reader.value()));
}

Result<std::unique_ptr<TermVectorsReader>> Lucene90CompressingTermVectorsReader::clone() const {
    auto reader = copy(false);
    if (!reader.has_value()) {
        return std::unexpected(std::move(reader.error()));
    }
    return std::unique_ptr<TermVectorsReader>(std::move(reader.value()));
}

VoidResult Lucene90CompressingTermVectorsReader::close() const {
    if (closed_) {
        return {};
    }
    closed_ = true;
    VoidResult result;
    if (index_reader_ != nullptr) {
        result = index_reader_->close();
    }
    if (vectors_stream_ != nullptr) {
        if (auto closed = vectors_stream_->close(); !closed.has_value() && result.has_value()) {
            result = closed;
        }
    }
    return result;
}

VoidResult Lucene90CompressingTermVectorsReader::prefetch(int32_t doc_id) {
    auto block_id = index_reader_->get_block_id(doc_id);
    if (!block_id.has_value()) {
        return std::unexpected(std::move(block_id.error()));
    }
    for (const int64_t cached : prefetched_block_id_cache_) {
        if (cached == block_id.value()) {
            return {};
        }
    }
    auto block_start = index_reader_->get_block_start_pointer(block_id.value());
    if (!block_start.has_value()) {
        return std::unexpected(std::move(block_start.error()));
    }
    auto block_length = index_reader_->get_block_length(block_id.value());
    if (!block_length.has_value()) {
        return std::unexpected(std::move(block_length.error()));
    }
    if (auto prefetched = vectors_stream_->prefetch(block_start.value(), block_length.value());
        !prefetched.has_value()) {
        return prefetched;
    }
    prefetched_block_id_cache_[prefetched_block_id_cache_index_ & PREFETCH_CACHE_MASK] = block_id.value();
    ++prefetched_block_id_cache_index_;
    return {};
}

// ---- decoding helpers ----

std::vector<std::vector<size_t>> Lucene90CompressingTermVectorsReader::position_index(
        size_t skip, size_t num_fields, const std::vector<size_t> &num_terms,
        const std::vector<size_t> &term_freqs) {
    std::vector<std::vector<size_t>> result(num_fields);
    size_t term_index = 0;
    for (size_t i = 0; i < skip; ++i) {
        term_index += num_terms[i];
    }
    for (size_t i = 0; i < num_fields; ++i) {
        const size_t term_count = num_terms[skip + i];
        std::vector<size_t> ends(term_count + 1, 0);
        size_t position = 0;
        for (size_t j = 0; j < term_count; ++j) {
            position += term_freqs[term_index + j];
            ends[j + 1] = position;
        }
        term_index += term_count;
        result[i] = std::move(ends);
    }
    return result;
}

Result<std::vector<std::vector<int32_t>>> Lucene90CompressingTermVectorsReader::read_positions(
        size_t skip, size_t num_fields, const std::vector<int32_t> &flags, const std::vector<size_t> &num_terms,
        const std::vector<size_t> &term_freqs, int32_t flag, size_t total_positions,
        const std::vector<std::vector<size_t>> &position_index) {
    std::vector<std::vector<int32_t>> result(num_fields);
    reader_->reset(total_positions);

    // Skip the fields that belong to the documents before `skip`.
    size_t to_skip = 0;
    size_t term_index = 0;
    for (size_t i = 0; i < skip; ++i) {
        if ((flags[i] & flag) != 0) {
            for (size_t j = 0; j < num_terms[i]; ++j) {
                to_skip += term_freqs[term_index + j];
            }
        }
        term_index += num_terms[i];
    }
    if (auto skipped = reader_->skip(to_skip, *vectors_stream_); !skipped.has_value()) {
        return std::unexpected(std::move(skipped.error()));
    }

    for (size_t i = 0; i < num_fields; ++i) {
        if ((flags[skip + i] & flag) == 0) {
            continue;
        }
        const size_t term_count = num_terms[skip + i];
        const size_t total_freq = position_index[i][term_count];
        std::vector<int32_t> field_positions;
        field_positions.reserve(total_freq);
        while (field_positions.size() < total_freq) {
            const size_t remaining = total_freq - field_positions.size();
            auto batch = reader_->next_batch(remaining, *vectors_stream_);
            if (!batch.has_value()) {
                return std::unexpected(std::move(batch.error()));
            }
            for (const int64_t value : batch.value()) {
                field_positions.push_back(static_cast<int32_t>(value));
            }
        }
        result[i] = std::move(field_positions);
    }

    if (auto skipped = reader_->skip(total_positions - reader_->ord(), *vectors_stream_); !skipped.has_value()) {
        return std::unexpected(std::move(skipped.error()));
    }
    return result;
}

// ---- get ----

Result<std::unique_ptr<Fields>> Lucene90CompressingTermVectorsReader::get(int32_t doc) {
    if (auto open = ensure_open(); !open.has_value()) {
        return std::unexpected(std::move(open.error()));
    }
    DataInput &in = *vectors_stream_;

    // Seek to the chunk that holds `doc`.
    size_t start_pointer = 0;
    if (is_loaded(doc) && block_state_.start_pointer.has_value()) {
        start_pointer = block_state_.start_pointer.value();
    } else {
        auto pointer = index_reader_->get_start_pointer(doc);
        if (!pointer.has_value()) {
            return std::unexpected(std::move(pointer.error()));
        }
        start_pointer = pointer.value();
    }
    if (auto sought = vectors_stream_->seek(start_pointer); !sought.has_value()) {
        return std::unexpected(std::move(sought.error()));
    }

    auto doc_base_value = in.read_vint();
    auto token = in.read_vint();
    if (!doc_base_value.has_value() || !token.has_value()) {
        return err(LuceneError::corrupt_index("a term vectors chunk header is truncated"));
    }
    const int32_t doc_base = doc_base_value.value();
    const int32_t chunk_docs = static_cast<int32_t>(static_cast<uint32_t>(token.value()) >> 1);
    if (doc < doc_base || doc >= doc_base + chunk_docs || doc_base + chunk_docs > num_docs_) {
        return err(LuceneError::corrupt_index("docBase=" + std::to_string(doc_base) +
                                              ", chunkDocs=" + std::to_string(chunk_docs) +
                                              ", doc=" + std::to_string(doc)));
    }
    block_state_ = BlockState{start_pointer, doc_base, chunk_docs};

    size_t skip = 0;
    size_t num_fields = 0;
    size_t total_fields = 0;
    if (chunk_docs == 1) {
        auto fields_in_doc = in.read_vint();
        if (!fields_in_doc.has_value()) {
            return err(LuceneError::corrupt_index("a term vectors chunk header is truncated"));
        }
        num_fields = static_cast<size_t>(fields_in_doc.value());
        total_fields = num_fields;
    } else {
        reader_->reset(static_cast<size_t>(chunk_docs));
        size_t sum = 0;
        for (int32_t d = doc_base; d < doc; ++d) {
            auto count = reader_->next_value(in);
            if (!count.has_value()) {
                return std::unexpected(std::move(count.error()));
            }
            sum += static_cast<size_t>(count.value());
        }
        skip = sum;
        auto count = reader_->next_value(in);
        if (!count.has_value()) {
            return std::unexpected(std::move(count.error()));
        }
        num_fields = static_cast<size_t>(count.value());
        sum += num_fields;
        for (int32_t d = doc + 1; d < doc_base + chunk_docs; ++d) {
            auto rest = reader_->next_value(in);
            if (!rest.has_value()) {
                return std::unexpected(std::move(rest.error()));
            }
            sum += static_cast<size_t>(rest.value());
        }
        total_fields = sum;
    }

    if (num_fields == 0) {
        return std::unique_ptr<Fields>{};
    }

    // Distinct field numbers of the chunk.
    auto field_num_token = in.read_byte();
    if (!field_num_token.has_value()) {
        return err(LuceneError::corrupt_index("a term vectors chunk is truncated"));
    }
    const int32_t bits_per_field_num = field_num_token.value() & 0x1F;
    int32_t total_distinct_fields = field_num_token.value() >> 5;
    if (total_distinct_fields == 0x07) {
        auto extra = in.read_vint();
        if (!extra.has_value()) {
            return err(LuceneError::corrupt_index("a term vectors chunk is truncated"));
        }
        total_distinct_fields += extra.value();
    }
    total_distinct_fields += 1;

    auto decoded_field_nums =
            read_packed_stream(in, static_cast<size_t>(total_distinct_fields), bits_per_field_num);
    if (!decoded_field_nums.has_value()) {
        return std::unexpected(std::move(decoded_field_nums.error()));
    }
    std::vector<int32_t> field_nums;
    field_nums.reserve(decoded_field_nums.value().size());
    for (const int64_t value : decoded_field_nums.value()) {
        field_nums.push_back(static_cast<int32_t>(value));
    }

    // Field flags and the index of every field in the distinct field list.
    auto bits_per_off = DirectWriter::bits_required(static_cast<int64_t>(field_nums.size()) - 1);
    if (!bits_per_off.has_value()) {
        return std::unexpected(std::move(bits_per_off.error()));
    }
    auto field_off_length = in.read_vint();
    if (!field_off_length.has_value() || field_off_length.value() < 0) {
        return err(LuceneError::corrupt_index("a term vectors chunk is truncated"));
    }
    auto field_off_bytes = in.get_bytes(static_cast<size_t>(field_off_length.value()));
    if (!field_off_bytes.has_value()) {
        return std::unexpected(std::move(field_off_bytes.error()));
    }
    std::vector<size_t> all_field_num_offs(total_fields, 0);
    for (size_t i = 0; i < total_fields; ++i) {
        auto value = direct_get(field_off_bytes.value(), bits_per_off.value(), i);
        if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
        }
        all_field_num_offs[i] = static_cast<size_t>(value.value());
    }

    std::vector<int32_t> all_flags(total_fields, 0);
    auto flags_selector = in.read_vint();
    if (!flags_selector.has_value()) {
        return err(LuceneError::corrupt_index("a term vectors chunk is truncated"));
    }
    if (flags_selector.value() == 0) {
        // One flag per distinct field, expanded to one flag per field instance.
        auto length = in.read_vint();
        if (!length.has_value() || length.value() < 0) {
            return err(LuceneError::corrupt_index("a term vectors chunk is truncated"));
        }
        auto bytes = in.get_bytes(static_cast<size_t>(length.value()));
        if (!bytes.has_value()) {
            return std::unexpected(std::move(bytes.error()));
        }
        std::vector<int32_t> distinct_flags(field_nums.size(), 0);
        for (size_t i = 0; i < field_nums.size(); ++i) {
            auto value = direct_get(bytes.value(), flags_bits(), i);
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            distinct_flags[i] = static_cast<int32_t>(value.value());
        }
        for (size_t i = 0; i < total_fields; ++i) {
            if (all_field_num_offs[i] >= distinct_flags.size()) {
                return err(LuceneError::corrupt_index("a field number offset is out of range"));
            }
            all_flags[i] = distinct_flags[all_field_num_offs[i]];
        }
    } else if (flags_selector.value() == 1) {
        // One flag per field instance.
        auto length = in.read_vint();
        if (!length.has_value() || length.value() < 0) {
            return err(LuceneError::corrupt_index("a term vectors chunk is truncated"));
        }
        auto bytes = in.get_bytes(static_cast<size_t>(length.value()));
        if (!bytes.has_value()) {
            return std::unexpected(std::move(bytes.error()));
        }
        for (size_t i = 0; i < total_fields; ++i) {
            auto value = direct_get(bytes.value(), flags_bits(), i);
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            all_flags[i] = static_cast<int32_t>(value.value());
        }
    } else {
        return err(LuceneError::illegal_state("invalid flag selector: " + std::to_string(flags_selector.value())));
    }

    std::vector<size_t> field_num_offs(num_fields, 0);
    for (size_t i = 0; i < num_fields; ++i) {
        field_num_offs[i] = all_field_num_offs[skip + i];
    }

    // Number of terms per field.
    auto terms_bits = in.read_vint();
    auto terms_length = in.read_vint();
    if (!terms_bits.has_value() || !terms_length.has_value() || terms_length.value() < 0) {
        return err(LuceneError::corrupt_index("a term vectors chunk is truncated"));
    }
    auto term_counts = in.get_bytes(static_cast<size_t>(terms_length.value()));
    if (!term_counts.has_value()) {
        return std::unexpected(std::move(term_counts.error()));
    }
    std::vector<size_t> num_terms(total_fields, 0);
    size_t total_terms = 0;
    for (size_t i = 0; i < total_fields; ++i) {
        auto value = direct_get(term_counts.value(), terms_bits.value(), i);
        if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
        }
        num_terms[i] = static_cast<size_t>(value.value());
        total_terms += num_terms[i];
    }

    // Term lengths: the prefix lengths come first, the suffix lengths after them.
    size_t doc_off = 0;
    size_t doc_len = 0;
    size_t total_len = 0;
    std::vector<size_t> field_lengths(num_fields, 0);
    std::vector<std::vector<int32_t>> prefix_lengths(num_fields);
    std::vector<std::vector<int32_t>> suffix_lengths(num_fields);
    {
        size_t to_skip = 0;
        for (size_t i = 0; i < skip; ++i) {
            to_skip += num_terms[i];
        }

        reader_->reset(total_terms);
        if (auto skipped = reader_->skip(to_skip, in); !skipped.has_value()) {
            return std::unexpected(std::move(skipped.error()));
        }
        for (size_t i = 0; i < num_fields; ++i) {
            const size_t term_count = num_terms[skip + i];
            std::vector<int32_t> field_prefix(term_count, 0);
            size_t done = 0;
            while (done < term_count) {
                auto batch = reader_->next_batch(term_count - done, in);
                if (!batch.has_value()) {
                    return std::unexpected(std::move(batch.error()));
                }
                for (const int64_t value : batch.value()) {
                    field_prefix[done] = static_cast<int32_t>(value);
                    ++done;
                }
            }
            prefix_lengths[i] = std::move(field_prefix);
        }
        if (auto skipped = reader_->skip(total_terms - reader_->ord(), in); !skipped.has_value()) {
            return std::unexpected(std::move(skipped.error()));
        }

        reader_->reset(total_terms);
        for (size_t i = 0; i < skip; ++i) {
            for (size_t j = 0; j < num_terms[i]; ++j) {
                auto value = reader_->next_value(in);
                if (!value.has_value()) {
                    return std::unexpected(std::move(value.error()));
                }
                doc_off += static_cast<size_t>(value.value());
            }
        }
        for (size_t i = 0; i < num_fields; ++i) {
            const size_t term_count = num_terms[skip + i];
            std::vector<int32_t> field_suffix(term_count, 0);
            size_t done = 0;
            while (done < term_count) {
                auto batch = reader_->next_batch(term_count - done, in);
                if (!batch.has_value()) {
                    return std::unexpected(std::move(batch.error()));
                }
                for (const int64_t value : batch.value()) {
                    field_suffix[done] = static_cast<int32_t>(value);
                    ++done;
                }
            }
            size_t field_length = 0;
            for (const int32_t length : field_suffix) {
                field_length += static_cast<size_t>(length);
            }
            field_lengths[i] = field_length;
            doc_len += field_length;
            suffix_lengths[i] = std::move(field_suffix);
        }

        total_len = doc_off + doc_len;
        for (size_t i = skip + num_fields; i < total_fields; ++i) {
            for (size_t j = 0; j < num_terms[i]; ++j) {
                auto value = reader_->next_value(in);
                if (!value.has_value()) {
                    return std::unexpected(std::move(value.error()));
                }
                total_len += static_cast<size_t>(value.value());
            }
        }
    }

    // Frequencies are stored as `freq - 1`.
    std::vector<size_t> term_freqs(total_terms, 0);
    {
        reader_->reset(total_terms);
        size_t i = 0;
        while (i < total_terms) {
            auto batch = reader_->next_batch(total_terms - i, in);
            if (!batch.has_value()) {
                return std::unexpected(std::move(batch.error()));
            }
            for (const int64_t value : batch.value()) {
                term_freqs[i] = static_cast<size_t>(value) + 1;
                ++i;
            }
        }
    }

    // Total number of positions, offsets and payloads of the chunk.
    size_t total_positions = 0;
    size_t total_offsets = 0;
    size_t total_payloads = 0;
    {
        size_t term_index = 0;
        for (size_t i = 0; i < total_fields; ++i) {
            const int32_t field_flag = all_flags[i];
            for (size_t j = 0; j < num_terms[i]; ++j) {
                const size_t freq = term_freqs[term_index];
                ++term_index;
                if ((field_flag & TV_POSITIONS) != 0) {
                    total_positions += freq;
                }
                if ((field_flag & TV_OFFSETS) != 0) {
                    total_offsets += freq;
                }
                if ((field_flag & TV_PAYLOADS) != 0) {
                    total_payloads += freq;
                }
            }
        }
    }

    const auto indexes = position_index(skip, num_fields, num_terms, term_freqs);

    // Positions.
    std::vector<std::vector<int32_t>> positions(num_fields);
    if (total_positions > 0) {
        auto decoded = read_positions(skip, num_fields, all_flags, num_terms, term_freqs, TV_POSITIONS,
                                      total_positions, indexes);
        if (!decoded.has_value()) {
            return std::unexpected(std::move(decoded.error()));
        }
        positions = std::move(decoded.value());
    }

    // Offsets and lengths, both predicted from the average chars per term.
    std::vector<std::vector<int32_t>> start_offsets(num_fields);
    std::vector<std::vector<int32_t>> lengths(num_fields);
    if (total_offsets > 0) {
        std::vector<float> chars_per_term(field_nums.size(), 0.0F);
        for (float &value : chars_per_term) {
            auto bits = in.read_int();
            if (!bits.has_value()) {
                return std::unexpected(std::move(bits.error()));
            }
            value = std::bit_cast<float>(static_cast<uint32_t>(bits.value()));
        }

        auto decoded_offsets = read_positions(skip, num_fields, all_flags, num_terms, term_freqs, TV_OFFSETS,
                                              total_offsets, indexes);
        if (!decoded_offsets.has_value()) {
            return std::unexpected(std::move(decoded_offsets.error()));
        }
        auto decoded_lengths = read_positions(skip, num_fields, all_flags, num_terms, term_freqs, TV_OFFSETS,
                                             total_offsets, indexes);
        if (!decoded_lengths.has_value()) {
            return std::unexpected(std::move(decoded_lengths.error()));
        }
        start_offsets = std::move(decoded_offsets.value());
        lengths = std::move(decoded_lengths.value());

        for (size_t i = 0; i < num_fields; ++i) {
            if (!start_offsets[i].empty() && !positions[i].empty()) {
                const float chars_per_term_field = chars_per_term[field_num_offs[i]];
                const size_t count = std::min(start_offsets[i].size(), positions[i].size());
                for (size_t j = 0; j < count; ++j) {
                    start_offsets[i][j] += static_cast<int32_t>(chars_per_term_field *
                                                                static_cast<float>(positions[i][j]));
                }
            }

            if (!start_offsets[i].empty()) {
                const size_t term_count = num_terms[skip + i];
                for (size_t j = 0; j < term_count; ++j) {
                    // Delta-decode the start offsets and patch the lengths with the
                    // term lengths.
                    const int32_t term_length = prefix_lengths[i][j] + suffix_lengths[i][j];
                    const size_t pos_start = indexes[i][j];
                    const size_t pos_end = indexes[i][j + 1];
                    lengths[i][pos_start] += term_length;
                    for (size_t k = pos_start + 1; k < pos_end; ++k) {
                        start_offsets[i][k] += start_offsets[i][k - 1];
                        lengths[i][k] += term_length;
                    }
                }
            }
        }
    }

    // Delta-decode the positions.
    if (total_positions > 0) {
        for (size_t i = 0; i < num_fields; ++i) {
            if (positions[i].empty()) {
                continue;
            }
            const size_t term_count = num_terms[skip + i];
            for (size_t j = 0; j < term_count; ++j) {
                for (size_t k = indexes[i][j] + 1; k < indexes[i][j + 1]; ++k) {
                    positions[i][k] += positions[i][k - 1];
                }
            }
        }
    }

    // Payload lengths.
    std::vector<IntVector> payload_index(num_fields);
    size_t total_payload_length = 0;
    size_t payload_off = 0;
    size_t payload_len = 0;
    if (total_payloads > 0) {
        size_t term_index = 0;
        reader_->reset(total_payloads);
        for (size_t i = 0; i < skip; ++i) {
            const int32_t field_flag = all_flags[i];
            for (size_t j = 0; j < num_terms[i]; ++j) {
                if ((field_flag & TV_PAYLOADS) != 0) {
                    for (size_t k = 0; k < term_freqs[term_index + j]; ++k) {
                        auto value = reader_->next_value(in);
                        if (!value.has_value()) {
                            return std::unexpected(std::move(value.error()));
                        }
                        payload_off += static_cast<size_t>(value.value());
                    }
                }
            }
            term_index += num_terms[i];
        }
        total_payload_length = payload_off;

        for (size_t i = 0; i < num_fields; ++i) {
            const int32_t field_flag = all_flags[skip + i];
            const size_t term_count = num_terms[skip + i];
            if ((field_flag & TV_PAYLOADS) != 0) {
                const size_t total_freq = indexes[i][term_count];
                auto index = std::make_shared<std::vector<int32_t>>(total_freq + 1, 0);
                size_t pos_index = 0;
                (*index)[pos_index] = static_cast<int32_t>(payload_len);
                for (size_t j = 0; j < term_count; ++j) {
                    for (size_t k = 0; k < term_freqs[term_index + j]; ++k) {
                        auto value = reader_->next_value(in);
                        if (!value.has_value()) {
                            return std::unexpected(std::move(value.error()));
                        }
                        payload_len += static_cast<size_t>(value.value());
                        ++pos_index;
                        (*index)[pos_index] = static_cast<int32_t>(payload_len);
                    }
                }
                payload_index[i] = std::move(index);
            }
            term_index += term_count;
        }
        total_payload_length += payload_len;
        for (size_t i = skip + num_fields; i < total_fields; ++i) {
            const int32_t field_flag = all_flags[i];
            for (size_t j = 0; j < num_terms[i]; ++j) {
                if ((field_flag & TV_PAYLOADS) != 0) {
                    for (size_t k = 0; k < term_freqs[term_index + j]; ++k) {
                        auto value = reader_->next_value(in);
                        if (!value.has_value()) {
                            return std::unexpected(std::move(value.error()));
                        }
                        total_payload_length += static_cast<size_t>(value.value());
                    }
                }
            }
            term_index += num_terms[i];
        }
    }

    // Decompress the term suffixes and the payloads of this document.
    const size_t data_offset = doc_off + payload_off;
    const size_t data_length = doc_len + payload_len;
    std::vector<uint8_t> decompressed;
    if (auto result = decompressor_->decompress(in, static_cast<int32_t>(total_len + total_payload_length),
                                                static_cast<int32_t>(data_offset), static_cast<int32_t>(data_length),
                                                decompressed);
        !result.has_value()) {
        return std::unexpected(std::move(result.error()));
    }
    if (data_offset + data_length > decompressed.size()) {
        return err(LuceneError::corrupt_index("the decompressed term vectors chunk is too short"));
    }
    auto suffix_bytes = std::make_shared<std::vector<uint8_t>>(
            decompressed.begin() + static_cast<std::ptrdiff_t>(data_offset),
            decompressed.begin() + static_cast<std::ptrdiff_t>(data_offset + doc_len));
    auto payload_bytes = std::make_shared<std::vector<uint8_t>>(
            decompressed.begin() + static_cast<std::ptrdiff_t>(data_offset + doc_len),
            decompressed.begin() + static_cast<std::ptrdiff_t>(data_offset + data_length));

    auto data = std::make_shared<TVFieldsData>();
    data->field_nums = std::move(field_nums);
    data->field_flags.assign(all_flags.begin() + static_cast<std::ptrdiff_t>(skip),
                             all_flags.begin() + static_cast<std::ptrdiff_t>(skip + num_fields));
    data->field_num_offs = std::move(field_num_offs);
    data->num_terms.assign(num_terms.begin() + static_cast<std::ptrdiff_t>(skip),
                           num_terms.begin() + static_cast<std::ptrdiff_t>(skip + num_fields));
    data->field_lengths = std::move(field_lengths);
    data->positions.resize(num_fields);
    data->start_offsets.resize(num_fields);
    data->lengths.resize(num_fields);
    for (size_t i = 0; i < num_fields; ++i) {
        data->positions[i] = std::make_shared<std::vector<int32_t>>(std::move(positions[i]));
        data->start_offsets[i] = std::make_shared<std::vector<int32_t>>(std::move(start_offsets[i]));
        data->lengths[i] = std::make_shared<std::vector<int32_t>>(std::move(lengths[i]));
    }
    data->payload_bytes = std::move(payload_bytes);
    data->payload_index.resize(num_fields);
    for (size_t i = 0; i < num_fields; ++i) {
        data->payload_index[i] = payload_index[i] != nullptr
                                         ? std::move(payload_index[i])
                                         : std::make_shared<std::vector<int32_t>>();
    }
    data->suffix_bytes = std::move(suffix_bytes);
    data->field_infos = field_infos_;

    size_t term_index = 0;
    for (size_t i = 0; i < skip; ++i) {
        term_index += num_terms[i];
    }
    data->term_data.resize(num_fields);
    for (size_t i = 0; i < num_fields; ++i) {
        const size_t term_count = num_terms[skip + i];
        TVTermData &term_data = data->term_data[i];
        term_data.prefix_lengths = std::move(prefix_lengths[i]);
        term_data.suffix_lengths = std::move(suffix_lengths[i]);
        term_data.term_freqs.reserve(term_count);
        for (size_t j = 0; j < term_count; ++j) {
            term_data.term_freqs.push_back(static_cast<int32_t>(term_freqs[term_index + j]));
        }
        term_data.position_index = std::move(indexes[i]);
        term_index += term_count;
    }

    auto fields = TVFields::create(std::move(data));
    if (!fields.has_value()) {
        return std::unexpected(std::move(fields.error()));
    }
    return std::unique_ptr<Fields>(std::move(fields.value()));
}

} // namespace lucene
