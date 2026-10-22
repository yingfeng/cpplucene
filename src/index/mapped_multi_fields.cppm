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

export module lucene_core:mapped_multi_fields;

import :types;
import std;

import :attribute_source;
import :block_term_state;
import :doc_id_merger;
import :doc_map;
import :error;
import :fields;
import :impacts_enum;
import :merge_state;
import :multi_fields;
import :postings_enum;
import :terms;
import :terms_enum;

namespace lucene {

/// One input segment's postings for the current term, plus its doc map.
class MappingPostingsSub {
public:
    MappingPostingsSub(PostingsEnum *postings, const DocMap *doc_map) : postings_(postings), doc_map_(doc_map) {}

    [[nodiscard]] Result<int32_t> next_doc();
    [[nodiscard]] const DocMap &get_doc_map() const { return *doc_map_; }
    [[nodiscard]] PostingsEnum &postings() { return *postings_; }
    [[nodiscard]] const PostingsEnum &postings() const { return *postings_; }

private:
    PostingsEnum *postings_;
    const DocMap *doc_map_;
};

/// Exposes the postings of several sub-readers as one, remapping the doc ids into
/// the merged segment and dropping the deleted documents.
///
/// Port of `MappingMultiPostingsEnum`.  **Deviations:** `advance` is not
/// supported (a merge only iterates forward), and the reuse protocol of the reference implementation
/// (`take_multi_docs_and_positions_enum` / `reset`) is not ported: every call
/// builds a fresh enumeration.
export class MappingMultiPostingsEnum final : public PostingsEnum {
public:
    /// `field` is only used in error messages.
    [[nodiscard]] static Result<std::unique_ptr<MappingMultiPostingsEnum>> create(
            std::string field, const MergeStateMeta &merge_state_meta,
            std::unique_ptr<MultiPostingsEnum> multi_postings);

    [[nodiscard]] int32_t doc_id() const override { return doc_id_; }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t /*target*/) override;
    [[nodiscard]] Result<int64_t> cost() const override;

    [[nodiscard]] Result<int32_t> freq() override;
    [[nodiscard]] Result<int32_t> next_position() override;
    [[nodiscard]] Result<int32_t> start_offset() const override;
    [[nodiscard]] Result<int32_t> end_offset() const override;
    [[nodiscard]] Result<std::optional<std::span<const uint8_t>>> get_payload() const override;

private:
    MappingMultiPostingsEnum(std::string field, std::unique_ptr<MultiPostingsEnum> multi_postings,
                             DocIdMerger<MappingPostingsSub> merger);

    [[nodiscard]] PostingsEnum &current_postings();
    [[nodiscard]] const PostingsEnum &current_postings() const;

    std::string field_;
    /// Owns the sub enumerations the doc-id merger points into.
    std::unique_ptr<MultiPostingsEnum> multi_postings_;
    DocIdMerger<MappingPostingsSub> merger_;
    std::optional<size_t> current_;
    int32_t doc_id_{-1};
};

/// A [`Terms`] view that maps the doc ids of its postings into the merged
/// segment.
///
/// Port of `MappedMultiTerms`.
export class MappedMultiTerms final : public Terms {
public:
    MappedMultiTerms(std::string field, std::shared_ptr<const MergeStateMeta> merge_state_meta,
                     std::unique_ptr<Terms> inner);

    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> iterator() const override;
    [[nodiscard]] Result<int64_t> size() const override;
    [[nodiscard]] Result<int64_t> sum_total_term_freq() const override;
    [[nodiscard]] Result<int64_t> sum_doc_freq() const override;
    [[nodiscard]] Result<int32_t> doc_count() const override;
    [[nodiscard]] bool has_freqs() const override { return inner_->has_freqs(); }
    [[nodiscard]] bool has_offsets() const override { return inner_->has_offsets(); }
    [[nodiscard]] bool has_positions() const override { return inner_->has_positions(); }
    [[nodiscard]] bool has_payloads() const override { return inner_->has_payloads(); }

private:
    std::string field_;
    std::shared_ptr<const MergeStateMeta> merge_state_meta_;
    std::unique_ptr<Terms> inner_;
};

/// A [`TermsEnum`] whose postings are remapped into the merged segment.
///
/// Port of `MappedMultiTermsEnum`.
export class MappedMultiTermsEnum final : public TermsEnum {
public:
    MappedMultiTermsEnum(std::string field, std::shared_ptr<const MergeStateMeta> merge_state_meta,
                         std::unique_ptr<TermsEnum> inner);

    [[nodiscard]] Result<const BytesRef *> next() override;
    [[nodiscard]] Result<const BytesRef *> term() const override;
    [[nodiscard]] Result<int64_t> ord() const override { return inner_->ord(); }
    [[nodiscard]] Result<int32_t> doc_freq() override;
    [[nodiscard]] Result<int64_t> total_term_freq() override;

    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>> postings_with_flags(
            std::unique_ptr<PostingsEnum> reuse, int32_t flags) override;
    [[nodiscard]] Result<std::unique_ptr<ImpactsEnum>> impacts(int32_t flags) override;

    [[nodiscard]] Result<AttributeSource *> attributes() override { return inner_->attributes(); }
    [[nodiscard]] Result<AttributeSource *> attributes_mut() override { return inner_->attributes_mut(); }
    [[nodiscard]] Result<bool> seek_exact(std::span<const uint8_t> term) override { return inner_->seek_exact(term); }
    [[nodiscard]] Result<bool> prepare_seek_exact(std::span<const uint8_t> term) override {
        return inner_->prepare_seek_exact(term);
    }
    [[nodiscard]] Result<bool> get_prepare_seek_exact_status(std::span<const uint8_t> target) override {
        return inner_->get_prepare_seek_exact_status(target);
    }
    [[nodiscard]] Result<SeekStatus> seek_ceil(std::span<const uint8_t> term) override {
        return inner_->seek_ceil(term);
    }
    [[nodiscard]] VoidResult seek_exact_with_ord(int64_t ord) override { return inner_->seek_exact_with_ord(ord); }
    [[nodiscard]] VoidResult seek_exact_with_state(std::span<const uint8_t> term,
                                                   const TermStateEnum &state) override {
        return inner_->seek_exact_with_state(term, state);
    }
    [[nodiscard]] Result<TermStateEnum> term_state() override { return inner_->term_state(); }

private:
    std::string field_;
    std::shared_ptr<const MergeStateMeta> merge_state_meta_;
    std::unique_ptr<TermsEnum> inner_;
};

/// A [`Fields`] view over several segment readers that maps the postings doc ids
/// into the merged segment and drops the deleted documents.
///
/// Port of `MappedMultiFields`, the view `FieldsConsumer::merge` writes.
export class MappedMultiFields final : public Fields {
public:
    MappedMultiFields(std::shared_ptr<const MergeStateMeta> merge_state_meta, std::unique_ptr<MultiFields> inner);

    [[nodiscard]] Result<std::unique_ptr<FieldNameIterator>> iterator() const override;
    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view field) const override;
    [[nodiscard]] Result<int32_t> size() const override { return inner_->size(); }

private:
    std::shared_ptr<const MergeStateMeta> merge_state_meta_;
    std::unique_ptr<MultiFields> inner_;
};

} // namespace lucene
