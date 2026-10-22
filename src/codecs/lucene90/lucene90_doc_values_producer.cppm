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

export module lucene_core:lucene90_doc_values_producer;

import :types;
import std;

import :binary_doc_values;
import :direct_monotonic_reader;
import :doc_values_producer;
import :doc_values_skipper;
import :error;
import :field_infos;
import :index_input;
import :numeric_doc_values;
import :segment_info;
import :segment_read_state;
import :sorted_doc_values;
import :sorted_numeric_doc_values;
import :sorted_set_doc_values;

namespace lucene {

/// The `.dvm` metadata of one optional skip index.
///
/// Port of `DocValuesSkipperEntry`
/// (`codecs/lucene90/lucene90_doc_values_producer.rs`).
export struct DocValuesSkipperEntry {
    int64_t offset{0};
    int64_t length{0};
    int64_t max_value{0};
    int64_t min_value{0};
    int32_t doc_count{0};
    int32_t max_doc_id{0};
};

/// The `.dvm` metadata of a numeric field, also reused as the ordinal metadata
/// of a sorted field and as the values of a sorted numeric or sorted set field.
///
/// Port of `NumericEntry`.  The values are either dense (every document has one)
/// or sparse (paired with an [`IndexedDISI`]), and reading them needs at most one
/// of `table` (a look-up table of at most 256 distinct values), `block_shift`
/// (values split into blocks with their own bit width) and `gcd`
/// (greatest-common-divisor compression).
export struct NumericEntry {
    /// `-2`: no document has a value; `-1`: every document has one; otherwise
    /// the file pointer of the [`IndexedDISI`].
    int64_t docs_with_field_offset{-2};
    int64_t docs_with_field_length{0};
    int32_t jump_table_entry_count{-1};
    int8_t dense_rank_power{-1};
    int64_t num_values{0};
    /// The look-up table of distinct values, empty when there is none.
    std::vector<int64_t> table;
    /// `-1` when the values are not split into blocks, otherwise the shift of the
    /// block size.
    int32_t block_shift{-1};
    int8_t bits_per_value{0};
    int64_t min_value{0};
    int64_t gcd{0};
    int64_t values_offset{0};
    int64_t values_length{0};
    /// `-1` when the blocks have no jump table.
    int64_t value_jump_table_offset{-1};
};

/// The `.dvm` metadata of a binary field.
///
/// Port of `BinaryEntry`: the raw bytes, the doc-id set of the documents that
/// have a value and, when the values have different lengths, the end addresses of
/// the values as monotonic numerics.
export struct BinaryEntry {
    int64_t data_offset{0};
    int64_t data_length{0};
    int64_t docs_with_field_offset{-2};
    int64_t docs_with_field_length{0};
    int32_t jump_table_entry_count{-1};
    int8_t dense_rank_power{-1};
    int32_t num_docs_with_field{0};
    int32_t min_length{0};
    int32_t max_length{0};
    int64_t addresses_offset{0};
    int64_t addresses_length{0};
    /// Empty when the values have a fixed length.
    std::optional<DirectMonotonicMeta> addresses_meta;
};

/// The `.dvm` metadata of the term dictionary of a sorted or sorted set field.
///
/// Port of `TermsDictEntry`: the dictionary is a sequence of LZ4 blocks of 64
/// terms, each block starting with one uncompressed term used as the preset
/// dictionary of the compressed suffixes.  A reverse index of one term every 1024
/// terms makes lookups sub-linear.
export struct TermsDictEntry {
    int64_t terms_dict_size{0};
    std::optional<DirectMonotonicMeta> terms_addresses_meta;
    int32_t max_term_length{0};
    int32_t max_block_length{0};
    int64_t terms_data_offset{0};
    int64_t terms_data_length{0};
    int64_t terms_addresses_offset{0};
    int64_t terms_addresses_length{0};
    int32_t terms_dict_index_shift{0};
    std::optional<DirectMonotonicMeta> terms_index_addresses_meta;
    int64_t terms_index_offset{0};
    int64_t terms_index_length{0};
    int64_t terms_index_addresses_offset{0};
    int64_t terms_index_addresses_length{0};
};

/// A sorted field: ordinals plus the dictionary they point into.
export struct SortedEntry {
    NumericEntry ords_entry;
    TermsDictEntry terms_dict_entry;
};

/// A sorted numeric field: numeric values plus, when documents have several
/// values, the start addresses of each document's run of values.
export struct SortedNumericEntry {
    NumericEntry base;
    int32_t num_docs_with_field{0};
    int64_t addresses_offset{0};
    int64_t addresses_length{0};
    std::optional<DirectMonotonicMeta> addresses_meta;
};

/// A sorted set field: either the shape of a sorted field (single-valued) or
/// multi-valued ordinals plus a dictionary.
export struct SortedSetEntry {
    std::optional<SortedEntry> single_value_entry;
    /// The ordinals of a multi-valued field, including the start addresses of
    /// every document's run of ordinals.
    std::optional<SortedNumericEntry> ords_entry;
    std::optional<TermsDictEntry> terms_dict_entry;
};

/// Reads the `.dvd`/`.dvm` pair of one segment.
///
/// Port of `Lucene90DocValuesProducer`
/// (`codecs/lucene90/lucene90_doc_values_producer.rs`): the whole metadata file is
/// parsed up front into one entry per field, and every `get_*` call builds a
/// fresh cursor over the shared data file.
///
/// **Deviations** (C++ mechanics only):
///  * the reference implementation's per-compression-strategy reader classes collapse into one
///    [`LongValues`] implementation per strategy, selected at construction time;
///  * `get_merge_instance` clones the data input and shares the entries, because
///    the only difference in the reference implementation is the merge-optimized direct reader, which
///    this port does not have;
///  * `terms_enum` is not implemented (it needs the search layer), so
///    `lookup_term` uses the generic binary search over `lookup_ord`.
export class Lucene90DocValuesProducer final : public DocValuesProducer {
public:
    /// Opens the data and metadata files and parses the `.dvm` file.
    [[nodiscard]] static Result<std::unique_ptr<Lucene90DocValuesProducer>> create(
            const SegmentReadState &state, const SegmentInfo &segment_info, std::string_view data_codec,
            std::string_view data_extension, std::string_view meta_codec, std::string_view meta_extension);

    ~Lucene90DocValuesProducer() override;

    Lucene90DocValuesProducer(const Lucene90DocValuesProducer &) = delete;
    Lucene90DocValuesProducer &operator=(const Lucene90DocValuesProducer &) = delete;

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric(const FieldInfo &field) const override;

    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> get_binary(const FieldInfo &field) const override;

    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted(const FieldInfo &field) const override;

    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>> get_sorted_numeric(const FieldInfo &field) const override;

    [[nodiscard]] Result<std::unique_ptr<SortedSetDocValues>> get_sorted_set(const FieldInfo &field) const override;

    [[nodiscard]] Result<std::unique_ptr<DocValuesSkipper>> get_skipper(const FieldInfo &field) const override;

    [[nodiscard]] VoidResult check_integrity() const override;

    [[nodiscard]] VoidResult close() const override;

    [[nodiscard]] Result<std::unique_ptr<DocValuesProducer>> get_merge_instance() const override;

    /// The parsed metadata of `field_num`, or `nullptr`.
    [[nodiscard]] const NumericEntry *numeric_entry(int32_t field_num) const noexcept;
    [[nodiscard]] const BinaryEntry *binary_entry(int32_t field_num) const noexcept;
    [[nodiscard]] const SortedEntry *sorted_entry(int32_t field_num) const noexcept;
    [[nodiscard]] const SortedSetEntry *sorted_set_entry(int32_t field_num) const noexcept;
    [[nodiscard]] const SortedNumericEntry *sorted_numeric_entry(int32_t field_num) const noexcept;

private:
    Lucene90DocValuesProducer(std::shared_ptr<IndexInput> data, int32_t max_doc, int32_t version);

    /// `mutable` because [`CloseableRef::close`] is `const` and drops the handle.
    mutable std::shared_ptr<IndexInput> data_;
    std::map<int32_t, NumericEntry> numerics_;
    std::map<int32_t, BinaryEntry> binaries_;
    std::map<int32_t, SortedEntry> sorted_;
    std::map<int32_t, SortedSetEntry> sorted_sets_;
    std::map<int32_t, SortedNumericEntry> sorted_numerics_;
    std::map<int32_t, DocValuesSkipperEntry> skippers_;
    int32_t max_doc_{0};
    int32_t version_{0};
};

} // namespace lucene
