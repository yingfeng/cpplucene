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

module lucene_core:freq_prox_fields.impl;

import :freq_prox_fields;

import :attribute_source;
import :block_term_state;
import :byte_block_pool;
import :byte_slice_reader;
import :bytes_ref;
import :bytes_ref_block_pool;
import :bytes_ref_iterator;
import :debug;
import :doc_id_set_iterator;
import :error;
import :fields;
import :freq_prox_terms_writer_per_field;
import :impacts_enum;
import :index_types;
import :int_block_pool;
import :postings_enum;
import :term_state;
import :terms;
import :terms_enum;
import :types;

import std;

namespace lucene {

// ---------------------------------------------------------------------------
// FreqProxTermsEnum
// ---------------------------------------------------------------------------

FreqProxTermsEnum::FreqProxTermsEnum(std::shared_ptr<FreqProxTermsWriterPerField> terms,
                                     std::shared_ptr<IntBlockPool> int_pool,
                                     std::shared_ptr<ByteBlockPool> byte_pool)
        : terms_(std::move(terms)),
          int_pool_(std::move(int_pool)),
          byte_pool_(std::move(byte_pool)),
          num_terms_(terms_->base().get_num_terms()) {}

Result<const FreqProxPostingsArray *> FreqProxTermsEnum::postings_array() const {
    const auto *postings = terms_->base().postings_array();
    if (postings == nullptr) {
        return err(LuceneError::illegal_state("postings array is missing"));
    }
    const auto *freq_prox = dynamic_cast<const FreqProxPostingsArray *>(postings);
    if (freq_prox == nullptr) {
        return err(LuceneError::illegal_state("expected a FreqProx posting array"));
    }
    return freq_prox;
}

Result<BytesRefBlockPoolPosition> FreqProxTermsEnum::position_at(size_t index) {
    auto postings = postings_array();
    if (!postings.has_value()) {
        return std::unexpected(std::move(postings.error()));
    }
    const auto &ids = terms_->base().get_sorted_term_ids();
    if (index >= num_terms_ || index >= ids.size()) {
        return err(LuceneError::illegal_argument("term ordinal " + std::to_string(index) + " is out of bounds"));
    }
    const auto term_id = static_cast<size_t>(ids[index]);
    const auto text_start = postings.value()->text_starts.at(term_id);
    return terms_pool_.fill_bytes_ref(text_start, *byte_pool_);
}

void FreqProxTermsEnum::set_current_term(const BytesRefBlockPoolPosition &position) {
    auto bytes = terms_pool_.bytes_ref(position, *byte_pool_);
    if (bytes.has_value()) {
        current_term_ = std::move(bytes.value());
    }
}

Result<size_t> FreqProxTermsEnum::current_term_id() const {
    if (!ord_.has_value()) {
        return err(LuceneError::illegal_state("terms enum has no current term"));
    }
    const auto &ids = terms_->base().get_sorted_term_ids();
    if (ord_.value() >= num_terms_ || ord_.value() >= ids.size()) {
        return err(LuceneError::illegal_state("terms enum is past the last term"));
    }
    return static_cast<size_t>(ids[ord_.value()]);
}

Result<const BytesRef *> FreqProxTermsEnum::next() {
    const size_t ord = ord_.has_value() ? ord_.value() + 1 : 0;
    ord_ = ord;
    if (ord >= num_terms_) {
        return static_cast<const BytesRef *>(nullptr);
    }
    auto position = position_at(ord);
    if (!position.has_value()) {
        return std::unexpected(std::move(position.error()));
    }
    set_current_term(position.value());
    return &current_term_;
}

Result<AttributeSource *> FreqProxTermsEnum::attributes() { return &attributes_; }

Result<AttributeSource *> FreqProxTermsEnum::attributes_mut() { return &attributes_; }

Result<bool> FreqProxTermsEnum::seek_exact(std::span<const uint8_t> term) {
    auto status = seek_ceil(term);
    if (!status.has_value()) {
        return std::unexpected(std::move(status.error()));
    }
    return status.value() == SeekStatus::Found;
}

Result<bool> FreqProxTermsEnum::prepare_seek_exact(std::span<const uint8_t> /*term*/) { return true; }

Result<bool> FreqProxTermsEnum::get_prepare_seek_exact_status(std::span<const uint8_t> target) {
    return seek_exact(target);
}

Result<SeekStatus> FreqProxTermsEnum::seek_ceil(std::span<const uint8_t> text) {
    if (num_terms_ == 0) {
        ord_ = size_t{0};
        return SeekStatus::End;
    }

    int32_t lo = 0;
    auto hi = static_cast<int32_t>(num_terms_) - 1;
    std::optional<BytesRefBlockPoolPosition> last_position;

    while (hi >= lo) {
        const auto mid = (lo + hi) >> 1;
        const auto mid_index = static_cast<size_t>(mid);
        auto position = position_at(mid_index);
        if (!position.has_value()) {
            return std::unexpected(std::move(position.error()));
        }
        auto bytes = terms_pool_.bytes_ref(position.value(), *byte_pool_);
        if (!bytes.has_value()) {
            return std::unexpected(std::move(bytes.error()));
        }
        const auto cmp = bytes.value().compare_to(text);
        if (cmp < 0) {
            lo = mid + 1;
        } else if (cmp > 0) {
            hi = mid - 1;
        } else {
            set_current_term(position.value());
            ord_ = mid_index;
            return SeekStatus::Found;
        }
        last_position = position.value();
    }

    // Keep the last probe at End, and the previous term for an empty dictionary.
    const auto lo_index = static_cast<size_t>(lo);
    if (lo_index >= num_terms_ && last_position.has_value()) {
        set_current_term(last_position.value());
    }

    ord_ = lo_index;
    if (lo_index >= num_terms_) {
        return SeekStatus::End;
    }
    auto position = position_at(lo_index);
    if (!position.has_value()) {
        return std::unexpected(std::move(position.error()));
    }
    set_current_term(position.value());
    return SeekStatus::NotFound;
}

VoidResult FreqProxTermsEnum::seek_exact_with_ord(int64_t ord) {
    if (ord < 0) {
        // The reference implementation casts to `usize` and would index out of bounds; treat it as
        // "no current term" instead.
        ord_ = std::nullopt;
        current_term_ = BytesRef{};
        return {};
    }
    const auto index = static_cast<size_t>(ord);
    auto position = position_at(index);
    if (!position.has_value()) {
        return std::unexpected(std::move(position.error()));
    }
    ord_ = index;
    set_current_term(position.value());
    return {};
}

VoidResult FreqProxTermsEnum::seek_exact_with_state(std::span<const uint8_t> term, const TermStateEnum & /*state*/) {
    auto found = seek_exact(term);
    if (!found.has_value()) {
        return std::unexpected(std::move(found.error()));
    }
    if (!found.value()) {
        return err(LuceneError::illegal_argument("the term does not exist"));
    }
    return {};
}

Result<const BytesRef *> FreqProxTermsEnum::term() const { return &current_term_; }

Result<int64_t> FreqProxTermsEnum::ord() const { return ord_.has_value() ? static_cast<int64_t>(ord_.value()) : -1; }

Result<int32_t> FreqProxTermsEnum::doc_freq() {
    // Not stored per term, and it cannot be computed at merge time without an
    // extra pass through the postings.
    return err(LuceneError::unsupported_operation("doc_freq is unavailable while postings are buffered"));
}

Result<int64_t> FreqProxTermsEnum::total_term_freq() {
    return err(LuceneError::unsupported_operation("total_term_freq is unavailable while postings are buffered"));
}

Result<std::unique_ptr<PostingsEnum>> FreqProxTermsEnum::postings_with_flags(std::unique_ptr<PostingsEnum> reuse,
                                                                           int32_t flags) {
    const bool has_prox = terms_->has_prox();
    const bool has_offsets = terms_->has_offsets();
    const bool has_freq = terms_->has_freq();

    auto term_id = current_term_id();
    if (!term_id.has_value()) {
        return std::unexpected(std::move(term_id.error()));
    }
    const auto id = static_cast<int32_t>(term_id.value());

    if (feature_requested(flags, POSITIONS)) {
        if (!has_prox) {
            // The caller wants positions but they were not indexed; do not lie.
            return err(LuceneError::illegal_argument("did not index positions"));
        }
        if (!has_offsets && feature_requested(flags, OFFSETS)) {
            return err(LuceneError::illegal_argument("did not index offsets"));
        }

        std::unique_ptr<FreqProxPostingsEnum> enumeration;
        if (auto *existing = dynamic_cast<FreqProxPostingsEnum *>(reuse.get());
            existing != nullptr && existing->terms().get() == terms_.get()) {
            (void)reuse.release();
            enumeration.reset(existing);
        } else {
            enumeration = std::make_unique<FreqProxPostingsEnum>(terms_, int_pool_, byte_pool_);
        }
        if (auto resetted = enumeration->reset(id); !resetted.has_value()) {
            return std::unexpected(std::move(resetted.error()));
        }
        return std::unique_ptr<PostingsEnum>(std::move(enumeration));
    }

    if (!has_freq && feature_requested(flags, FREQS)) {
        return err(LuceneError::illegal_argument("did not index freq"));
    }

    std::unique_ptr<FreqProxDocsEnum> enumeration;
    if (auto *existing = dynamic_cast<FreqProxDocsEnum *>(reuse.get());
        existing != nullptr && existing->terms().get() == terms_.get()) {
        (void)reuse.release();
        enumeration.reset(existing);
    } else {
        enumeration = std::make_unique<FreqProxDocsEnum>(terms_, int_pool_, byte_pool_);
    }
    if (auto resetted = enumeration->reset(id); !resetted.has_value()) {
        return std::unexpected(std::move(resetted.error()));
    }
    return std::unique_ptr<PostingsEnum>(std::move(enumeration));
}

Result<std::unique_ptr<ImpactsEnum>> FreqProxTermsEnum::impacts(int32_t /*flags*/) {
    return err(LuceneError::unsupported_operation("impacts are unavailable while postings are buffered"));
}

Result<TermStateEnum> FreqProxTermsEnum::term_state() {
    const auto current_ord = ord();
    if (!current_ord.has_value()) {
        return std::unexpected(std::move(current_ord.error()));
    }
    return TermStateEnum(OrdTermState(current_ord.value()));
}

// ---------------------------------------------------------------------------
// FreqProxTerms
// ---------------------------------------------------------------------------

FreqProxTerms::FreqProxTerms(std::shared_ptr<FreqProxTermsWriterPerField> terms,
                             std::shared_ptr<IntBlockPool> int_pool, std::shared_ptr<ByteBlockPool> byte_pool)
        : terms_(std::move(terms)), int_pool_(std::move(int_pool)), byte_pool_(std::move(byte_pool)) {}

Result<std::unique_ptr<TermsEnum>> FreqProxTerms::iterator() const {
    auto enumeration = std::make_unique<FreqProxTermsEnum>(terms_, int_pool_, byte_pool_);
    enumeration->reset();
    return std::unique_ptr<TermsEnum>(std::move(enumeration));
}

Result<int64_t> FreqProxTerms::size() const {
    return err(LuceneError::unsupported_operation("size is unavailable while postings are buffered"));
}

Result<int64_t> FreqProxTerms::sum_total_term_freq() const {
    return err(LuceneError::unsupported_operation("sum_total_term_freq is unavailable while postings are buffered"));
}

Result<int64_t> FreqProxTerms::sum_doc_freq() const {
    return err(LuceneError::unsupported_operation("sum_doc_freq is unavailable while postings are buffered"));
}

Result<int32_t> FreqProxTerms::doc_count() const {
    return err(LuceneError::unsupported_operation("doc_count is unavailable while postings are buffered"));
}

bool FreqProxTerms::has_freqs() const {
    return terms_->base().index_options() >= IndexOptions::DocsAndFreqs;
}

bool FreqProxTerms::has_offsets() const {
    // The in-memory buffer may hold offsets because the `FieldInfo` said so when
    // the field was created, but they may have been downgraded while indexing.
    return terms_->base().index_options() >= IndexOptions::DocsAndFreqsAndPositionsAndOffsets;
}

bool FreqProxTerms::has_positions() const {
    return terms_->base().index_options() >= IndexOptions::DocsAndFreqsAndPositions;
}

bool FreqProxTerms::has_payloads() const { return terms_->saw_payloads(); }

// ---------------------------------------------------------------------------
// FreqProxFieldNameIterator / FreqProxFields
// ---------------------------------------------------------------------------

Result<const std::string *> FreqProxFieldNameIterator::next() {
    if (pos_ >= fields_.size()) {
        return static_cast<const std::string *>(nullptr);
    }
    const auto *name = &fields_[pos_]->field_info()->name;
    pos_ += 1;
    return name;
}

Result<std::unique_ptr<FieldNameIterator>> FreqProxFields::iterator() const {
    return std::unique_ptr<FieldNameIterator>(std::make_unique<FreqProxFieldNameIterator>(fields_, 0));
}

Result<std::unique_ptr<Terms>> FreqProxFields::terms(std::string_view field) const {
    // The fields are sorted by name, so a binary search finds the per-field
    // writer.
    size_t lo = 0;
    auto hi = static_cast<int64_t>(fields_.size()) - 1;
    while (hi >= static_cast<int64_t>(lo)) {
        const auto mid = static_cast<size_t>((static_cast<int64_t>(lo) + hi) >> 1);
        const auto &name = fields_[mid]->field_info()->name;
        if (name < field) {
            lo = mid + 1;
        } else if (name > field) {
            hi = static_cast<int64_t>(mid) - 1;
        } else {
            return std::unique_ptr<Terms>(std::make_unique<FreqProxTerms>(fields_[mid], int_pool_, byte_pool_));
        }
    }
    return static_cast<std::unique_ptr<Terms>>(nullptr);
}

Result<int32_t> FreqProxFields::size() const {
    return err(LuceneError::unsupported_operation("the buffered fields have no known size"));
}

// ---------------------------------------------------------------------------
// FreqProxDocsEnum
// ---------------------------------------------------------------------------

FreqProxDocsEnum::FreqProxDocsEnum(std::shared_ptr<FreqProxTermsWriterPerField> terms,
                                   std::shared_ptr<IntBlockPool> int_pool, std::shared_ptr<ByteBlockPool> byte_pool)
        : terms_(std::move(terms)),
          int_pool_(std::move(int_pool)),
          byte_pool_(std::move(byte_pool)),
          reader_(*byte_pool_),
          read_term_freq_(terms_->has_freq()) {}

VoidResult FreqProxDocsEnum::reset(int32_t term_id) {
    const auto term_index = static_cast<size_t>(term_id);
    term_id_ = term_id != -1 ? std::optional<size_t>(term_index) : std::nullopt;
    if (auto initialized = terms_->base().init_reader(reader_, term_index, 0, *int_pool_); !initialized.has_value()) {
        return initialized;
    }
    ended_ = false;
    doc_id_ = -1;
    return {};
}

Result<int32_t> FreqProxDocsEnum::next_doc() {
    if (doc_id_ == -1) {
        doc_id_ = 0;
    }

    if (reader_.eof()) {
        if (ended_) {
            return NO_MORE_DOCS;
        }
        ended_ = true;
        const auto *postings = terms_->base().postings_array();
        const auto *freq_prox = dynamic_cast<const FreqProxPostingsArray *>(postings);
        if (freq_prox == nullptr) {
            return err(LuceneError::illegal_state("expected a FreqProx posting array"));
        }
        const auto term_id = term_id_;
        if (!term_id.has_value()) {
            return err(LuceneError::illegal_state("postings enum has no current term"));
        }
        doc_id_ = freq_prox->last_doc_ids[term_id.value()];
        if (read_term_freq_) {
            if (!freq_prox->term_freqs.has_value()) {
                return err(LuceneError::illegal_state("term_freqs not available"));
            }
            freq_ = freq_prox->term_freqs.value()[term_id.value()];
        }
    } else {
        auto code = reader_.read_vint();
        if (!code.has_value()) {
            return std::unexpected(std::move(code.error()));
        }
        if (!read_term_freq_) {
            doc_id_ += code.value();
        } else {
            doc_id_ += static_cast<int32_t>(static_cast<uint32_t>(code.value()) >> 1);
            if ((code.value() & 1) != 0) {
                freq_ = 1;
            } else {
                auto read = reader_.read_vint();
                if (!read.has_value()) {
                    return std::unexpected(std::move(read.error()));
                }
                freq_ = read.value();
            }
        }
    }
    return doc_id_;
}

Result<int32_t> FreqProxDocsEnum::advance(int32_t /*target*/) {
    return err(LuceneError::unsupported_operation("advance is not supported on the buffered postings"));
}

Result<int64_t> FreqProxDocsEnum::cost() const {
    return err(LuceneError::unsupported_operation("cost is unknown for the buffered postings"));
}

Result<int32_t> FreqProxDocsEnum::freq() {
    // Do not lie here: codecs would write heaps of wasted 1s into the index.
    if (!read_term_freq_) {
        return err(LuceneError::illegal_state("freq was not indexed"));
    }
    return freq_;
}

Result<int32_t> FreqProxDocsEnum::next_position() { return -1; }

Result<int32_t> FreqProxDocsEnum::start_offset() const { return -1; }

Result<int32_t> FreqProxDocsEnum::end_offset() const { return -1; }

Result<std::optional<std::span<const uint8_t>>> FreqProxDocsEnum::get_payload() const {
    return std::optional<std::span<const uint8_t>>{};
}

// ---------------------------------------------------------------------------
// FreqProxPostingsEnum
// ---------------------------------------------------------------------------

FreqProxPostingsEnum::FreqProxPostingsEnum(std::shared_ptr<FreqProxTermsWriterPerField> terms,
                                           std::shared_ptr<IntBlockPool> int_pool,
                                           std::shared_ptr<ByteBlockPool> byte_pool)
        : terms_(std::move(terms)),
          int_pool_(std::move(int_pool)),
          byte_pool_(std::move(byte_pool)),
          reader_(*byte_pool_),
          pos_reader_(*byte_pool_),
          read_offsets_(terms_->has_offsets()) {
    debug_check(terms_->has_prox(), "positions must be indexed");
    debug_check(terms_->has_freq(), "frequencies must be indexed");
}

VoidResult FreqProxPostingsEnum::reset(int32_t term_id) {
    term_id_ = static_cast<size_t>(term_id);
    if (auto initialized = terms_->base().init_reader(reader_, term_id_, 0, *int_pool_); !initialized.has_value()) {
        return initialized;
    }
    if (auto initialized = terms_->base().init_reader(pos_reader_, term_id_, 1, *int_pool_);
        !initialized.has_value()) {
        return initialized;
    }
    ended_ = false;
    doc_id_ = -1;
    pos_left_ = 0;
    return {};
}

Result<int32_t> FreqProxPostingsEnum::next_doc() {
    if (doc_id_ == -1) {
        doc_id_ = 0;
    }

    while (pos_left_ != 0) {
        if (auto skipped = next_position(); !skipped.has_value()) {
            return std::unexpected(std::move(skipped.error()));
        }
    }

    if (reader_.eof()) {
        if (ended_) {
            return NO_MORE_DOCS;
        }
        ended_ = true;
        const auto *postings = terms_->base().postings_array();
        const auto *freq_prox = dynamic_cast<const FreqProxPostingsArray *>(postings);
        if (freq_prox == nullptr) {
            return err(LuceneError::illegal_state("expected a FreqProx posting array"));
        }
        doc_id_ = freq_prox->last_doc_ids[term_id_];
        if (!freq_prox->term_freqs.has_value()) {
            return err(LuceneError::illegal_state("term_freqs not available"));
        }
        freq_ = freq_prox->term_freqs.value()[term_id_];
    } else {
        auto code = reader_.read_vint();
        if (!code.has_value()) {
            return std::unexpected(std::move(code.error()));
        }
        doc_id_ += static_cast<int32_t>(static_cast<uint32_t>(code.value()) >> 1);
        if ((code.value() & 1) != 0) {
            freq_ = 1;
        } else {
            auto read = reader_.read_vint();
            if (!read.has_value()) {
                return std::unexpected(std::move(read.error()));
            }
            freq_ = read.value();
        }
    }

    pos_left_ = freq_;
    pos_ = 0;
    start_offset_ = 0;
    return doc_id_;
}

Result<int32_t> FreqProxPostingsEnum::advance(int32_t /*target*/) {
    return err(LuceneError::unsupported_operation("advance is not supported on the buffered postings"));
}

Result<int64_t> FreqProxPostingsEnum::cost() const {
    return err(LuceneError::unsupported_operation("cost is unknown for the buffered postings"));
}

Result<int32_t> FreqProxPostingsEnum::freq() { return freq_; }

Result<int32_t> FreqProxPostingsEnum::next_position() {
    debug_check(pos_left_ > 0, "next_position must not be called more than freq times");
    pos_left_ -= 1;

    auto code = pos_reader_.read_vint();
    if (!code.has_value()) {
        return std::unexpected(std::move(code.error()));
    }
    pos_ += static_cast<int32_t>(static_cast<uint32_t>(code.value()) >> 1);

    if ((code.value() & 1) != 0) {
        has_payload_ = true;
        auto payload_len = pos_reader_.read_vint();
        if (!payload_len.has_value()) {
            return std::unexpected(std::move(payload_len.error()));
        }
        payload_.resize(static_cast<size_t>(payload_len.value()));
        if (!payload_.empty()) {
            if (auto read = pos_reader_.read_bytes(payload_); !read.has_value()) {
                return std::unexpected(std::move(read.error()));
            }
        }
    } else {
        has_payload_ = false;
    }

    if (read_offsets_) {
        auto start_delta = pos_reader_.read_vint();
        if (!start_delta.has_value()) {
            return std::unexpected(std::move(start_delta.error()));
        }
        start_offset_ += start_delta.value();
        auto length = pos_reader_.read_vint();
        if (!length.has_value()) {
            return std::unexpected(std::move(length.error()));
        }
        end_offset_ = start_offset_ + length.value();
    }
    return pos_;
}

Result<int32_t> FreqProxPostingsEnum::start_offset() const {
    if (!read_offsets_) {
        return err(LuceneError::illegal_state("offsets were not indexed"));
    }
    return start_offset_;
}

Result<int32_t> FreqProxPostingsEnum::end_offset() const {
    if (!read_offsets_) {
        return err(LuceneError::illegal_state("offsets were not indexed"));
    }
    return end_offset_;
}

Result<std::optional<std::span<const uint8_t>>> FreqProxPostingsEnum::get_payload() const {
    if (has_payload_) {
        return std::optional<std::span<const uint8_t>>(std::span<const uint8_t>(payload_));
    }
    return std::optional<std::span<const uint8_t>>{};
}

} // namespace lucene
