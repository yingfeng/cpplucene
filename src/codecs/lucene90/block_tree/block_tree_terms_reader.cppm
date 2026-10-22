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

export module lucene_core:block_tree_terms_reader;

import :types;
import std;

import :block_tree_segment_terms_enum;
import :error;
import :field_infos;
import :fields;
import :fields_producer;
import :index_input;
import :postings_reader_base;
import :segment_info;
import :segment_read_state;
import :terms;

namespace lucene {

/// The block tree state shared by all field readers of a segment: the `.tim`
/// input, the postings reader, the segment name and the format version.
///
/// Port of `TermsReader`
/// (`core/codecs/lucene90/block_tree/lucene90_block_tree_terms_reader.rs`).
///
/// **Deviation:** rlucene's `TermsReader` is an `Arc` that the field readers
/// hold; here it implements [`BlockTreeTermsParent`], which keeps the field
/// readers free of a dependency on this partition (no module cycle).
export class TermsReader : public BlockTreeTermsParent {
public:
    TermsReader(std::unique_ptr<IndexInput> terms_in, std::unique_ptr<PostingsReaderBase> postings_reader,
                std::string segment, int32_t version);

    [[nodiscard]] Result<std::unique_ptr<IndexInput>> clone_terms_input() const override;
    [[nodiscard]] PostingsReaderBase &postings_reader() override { return *postings_reader_; }
    [[nodiscard]] const std::string &segment_name() const override { return segment_; }
    [[nodiscard]] int32_t version() const override { return version_; }

    [[nodiscard]] IndexInput &terms_in() noexcept { return *terms_in_; }
    [[nodiscard]] VoidResult close();

private:
    std::unique_ptr<IndexInput> terms_in_;
    std::unique_ptr<PostingsReaderBase> postings_reader_;
    std::string segment_;
    int32_t version_;
};

/// A block-based terms index and dictionary: a prefix trie over the terms whose
/// leaves are blocks of term suffixes.
///
/// Port of `Lucene90BlockTreeTermsReader`
/// (`core/codecs/lucene90/block_tree/lucene90_block_tree_terms_reader.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * {@link field_names} is kept as a convenience next to the [`Fields`]
///    surface, and {@link field_count} carries the old `size()` (which now has to
///    return `Result<int32_t>` to satisfy [`Fields::size`]);
///  * `terms()` returns the type-erased `std::unique_ptr<Terms>`; the concrete
///    `FieldReader` is still what is handed out, so callers that need it can
///    `dynamic_cast`;
///  * `get_merge_instance` keeps the [`FieldsProducer`] default (none);
///  * the index input is opened with `state.context` rather than with the extra
///    `ReadAdvice::RandomPreload` read advice.
export class Lucene90BlockTreeTermsReader final : public FieldsProducer {
public:
    /// Opens `<segment>[_suffix].tim/.tip/.tmd` and reads the dictionary.
    [[nodiscard]] static Result<std::unique_ptr<Lucene90BlockTreeTermsReader>> create(
            std::unique_ptr<PostingsReaderBase> postings_reader, SegmentReadState &state,
            const SegmentInfo &segment_info);

    /// Names of all fields that have terms, sorted.
    [[nodiscard]] const std::vector<std::string> &field_names() const noexcept { return field_list_; }

    /// Number of fields that have terms (the old, allocation-free `size`).
    [[nodiscard]] size_t field_count() const noexcept { return field_map_.size(); }

    // ---- Fields ----

    /// Steps through the field names, in sorted order.
    [[nodiscard]] Result<std::unique_ptr<FieldNameIterator>> iterator() const override;

    /// The [`Terms`] of `field`, or `nullptr` when the field has no terms.
    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view field) const override;

    /// Number of fields that have terms.
    [[nodiscard]] Result<int32_t> size() const override;

    // ---- FieldsProducer ----

    [[nodiscard]] VoidResult check_integrity() const override;

    [[nodiscard]] VoidResult close() const override;

    [[nodiscard]] std::string to_string() const;

private:
    mutable std::shared_ptr<TermsReader> terms_reader_;
    mutable std::shared_ptr<IndexInput> index_in_;
    mutable std::map<int32_t, std::unique_ptr<FieldReader>> field_map_;
    std::vector<std::string> field_list_;
    std::shared_ptr<const FieldInfos> field_infos_;
};

} // namespace lucene
