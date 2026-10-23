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

export module lucene_core:index_sorter;

import :types;
import std;

import :error;
import :leaf_reader;
import :number;

namespace lucene {

/// Orders the documents of one segment.
///
/// Port of the `DocComparator` trait (`core/index/index_sorter.rs`).
export class DocComparator {
public:
    virtual ~DocComparator() = default;

    /// A negative value when `doc_id1` sorts before `doc_id2`, zero when they are
    /// equal, a positive value otherwise.
    [[nodiscard]] virtual int32_t compare(int32_t doc_id1, int32_t doc_id2) const = 0;
};

/// Applies several comparators in priority order: the first one that reports a
/// difference decides.
///
/// Port of the comparator list `Sorter::sort` takes in
/// `core/index/sorter.rs`, which an index sort with more than one field needs.
export class ChainedDocComparator final : public DocComparator {
public:
    /// Chains `comparators`, which must not be empty.
    [[nodiscard]] static Result<std::unique_ptr<ChainedDocComparator>> create(
            std::vector<std::unique_ptr<DocComparator>> comparators);

    [[nodiscard]] int32_t compare(int32_t doc_id1, int32_t doc_id2) const override;

private:
    explicit ChainedDocComparator(std::vector<std::unique_ptr<DocComparator>> comparators);

    std::vector<std::unique_ptr<DocComparator>> comparators_;
};

/// The old-to-new and new-to-old document id permutations of a sorted segment.
///
/// Port of the `DocMap` trait of `core/index/index_sorter.rs`.
///
/// **Deviation:** the reference implementation has two unrelated `DocMap` traits
/// (this one and the merge-state one); the port keeps the merge-state
/// [`DocMap`](:doc_map) name and calls this one `IndexSortDocMap`.
export class IndexSortDocMap {
public:
    virtual ~IndexSortDocMap() = default;

    /// How many documents the map covers.
    [[nodiscard]] virtual int32_t size() const noexcept = 0;

    /// The position `old_doc` takes in the sorted segment.
    [[nodiscard]] virtual Result<int32_t> old_to_new(int32_t old_doc) const = 0;

    /// The document that sorts into position `new_doc`.
    [[nodiscard]] virtual Result<int32_t> new_to_old(int32_t new_doc) const = 0;
};

/// An [`IndexSortDocMap`] holding both permutations as plain arrays.
///
/// Port of `IndexSortDocMapImpl` (`core/index/index_sorter.rs`).
///
/// **Deviation:** the reference implementation stores the permutations in
/// monotonic packed long buffers to save memory when the segment is nearly
/// sorted; the port keeps two `int32_t` arrays, which is simpler and bounded by
/// the segment size.
export class IndexSortDocMapImpl final : public IndexSortDocMap {
public:
    /// `new_to_old` lists the documents in their new order; the map covers as
    /// many old documents as it has entries.
    explicit IndexSortDocMapImpl(std::vector<int32_t> new_to_old);

    /// The same, for a segment whose old document space is larger than the number
    /// of documents that survived (the extra ones are reported as deleted).
    IndexSortDocMapImpl(std::vector<int32_t> new_to_old, int32_t old_doc_count);

    /// The number of old documents, i.e. the segment's `max_doc`.
    [[nodiscard]] int32_t size() const noexcept override;
    /// The number of documents that survived the sort.
    [[nodiscard]] int32_t new_doc_count() const noexcept;
    [[nodiscard]] Result<int32_t> old_to_new(int32_t old_doc) const override;
    [[nodiscard]] Result<int32_t> new_to_old(int32_t new_doc) const override;

    [[nodiscard]] const std::vector<int32_t> &new_to_old_values() const noexcept { return new_to_old_; }
    [[nodiscard]] const std::vector<int32_t> &old_to_new_values() const noexcept { return old_to_new_; }

private:
    std::vector<int32_t> new_to_old_;
    std::vector<int32_t> old_to_new_;
};

/// Reads one document's sort value as a `long` that sorts in the same order as
/// the original value, so documents from different segments can be compared.
///
/// Port of the `ComparableProvider` trait (`core/index/index_sorter.rs`).
export class ComparableProvider {
public:
    virtual ~ComparableProvider() = default;

    /// The sort key of `doc_id`; a document without a value gets the configured
    /// missing value.
    [[nodiscard]] virtual Result<int64_t> get_as_comparable_long(int32_t doc_id) = 0;
};

/// Decides how the documents of a segment should be sorted.
///
/// Port of the `IndexSorter` trait (`core/index/index_sorter.rs`).
///
/// **Deviation:** the reference implementation builds the cross-segment
/// `ComparableProvider`s through an `OrdinalMap` built from the per-segment term
/// dictionaries; the port implements that for the numeric sorters only, so
/// `StringIndexSorter` cannot take part in a merge yet (it reports
/// `unsupported_operation`).
export class IndexSorter {
public:
    virtual ~IndexSorter() = default;

    /// A short name for this sorter (`int`, `long`, `string`, ...), used by
    /// [`to_string`].
    ///
    /// **Deviation:** the reference implementation returns the name of the
    /// `SortFieldProvider` here, because that is what it writes into the segment
    /// metadata.  The port's sorters are coarser (one per value type, not per
    /// sort-field flavour), so the metadata uses `sort_field_provider_name`
    /// (`:sort_field_provider`) instead and this name is only for diagnostics.
    [[nodiscard]] virtual std::string_view provider_name() const noexcept = 0;

    /// The value a document without the field sorts as, when the sorter has a
    /// numeric one.
    [[nodiscard]] virtual std::optional<Number> missing_value() const noexcept = 0;

    /// The comparator over the documents of `leaf_reader`.
    [[nodiscard]] virtual Result<std::unique_ptr<DocComparator>> doc_comparator(LeafReader &leaf_reader,
                                                                               int32_t max_doc) const = 0;

    /// The cross-segment sort keys of `leaf_reader`'s documents.
    ///
    /// The default reports `unsupported_operation`: a sorter that cannot produce
    /// comparable keys (a term sorter without a global ordinal space, for example)
    /// can only be used within one segment.
    [[nodiscard]] virtual Result<std::unique_ptr<ComparableProvider>> comparable_provider(
            LeafReader &leaf_reader) const;

    /// `${provider_name}:${field}`, with `!` appended when the order is reversed.
    [[nodiscard]] std::string to_string() const;
};

/// Sorts the documents of a leaf by `int` doc values.
export class IntIndexSorter final : public IndexSorter {
public:
    [[nodiscard]] static Result<IntIndexSorter> create(std::string field, std::optional<int32_t> missing_value,
                                                 bool reverse);

    [[nodiscard]] std::string_view provider_name() const noexcept override { return "int"; }
    [[nodiscard]] std::optional<Number> missing_value() const noexcept override;
    [[nodiscard]] Result<std::unique_ptr<DocComparator>> doc_comparator(LeafReader &leaf_reader,
                                                                       int32_t max_doc) const override;
    [[nodiscard]] Result<std::unique_ptr<ComparableProvider>> comparable_provider(
            LeafReader &leaf_reader) const override;

    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    [[nodiscard]] bool reverse() const noexcept { return reverse_mul_ < 0; }

private:
    IntIndexSorter(std::string field, std::optional<int32_t> missing_value, int32_t reverse_mul);

    std::string field_;
    std::optional<int32_t> missing_value_;
    int32_t reverse_mul_{1};
};

/// Sorts the documents of a leaf by `long` doc values.
export class LongIndexSorter final : public IndexSorter {
public:
    [[nodiscard]] static Result<LongIndexSorter> create(std::string field, std::optional<int64_t> missing_value,
                                                  bool reverse);

    [[nodiscard]] std::string_view provider_name() const noexcept override { return "long"; }
    [[nodiscard]] std::optional<Number> missing_value() const noexcept override;
    [[nodiscard]] Result<std::unique_ptr<DocComparator>> doc_comparator(LeafReader &leaf_reader,
                                                                       int32_t max_doc) const override;
    [[nodiscard]] Result<std::unique_ptr<ComparableProvider>> comparable_provider(
            LeafReader &leaf_reader) const override;

    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    [[nodiscard]] bool reverse() const noexcept { return reverse_mul_ < 0; }

private:
    LongIndexSorter(std::string field, std::optional<int64_t> missing_value, int32_t reverse_mul);

    std::string field_;
    std::optional<int64_t> missing_value_;
    int32_t reverse_mul_{1};
};

/// Sorts the documents of a leaf by `float` doc values (raw bits, as the float
/// doc values field stores them).
export class FloatIndexSorter final : public IndexSorter {
public:
    [[nodiscard]] static Result<FloatIndexSorter> create(std::string field, std::optional<float> missing_value,
                                                   bool reverse);

    [[nodiscard]] std::string_view provider_name() const noexcept override { return "float"; }
    [[nodiscard]] std::optional<Number> missing_value() const noexcept override;
    [[nodiscard]] Result<std::unique_ptr<DocComparator>> doc_comparator(LeafReader &leaf_reader,
                                                                       int32_t max_doc) const override;
    [[nodiscard]] Result<std::unique_ptr<ComparableProvider>> comparable_provider(
            LeafReader &leaf_reader) const override;

    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    [[nodiscard]] bool reverse() const noexcept { return reverse_mul_ < 0; }

private:
    FloatIndexSorter(std::string field, std::optional<float> missing_value, int32_t reverse_mul);

    std::string field_;
    std::optional<float> missing_value_;
    int32_t reverse_mul_{1};
};

/// Sorts the documents of a leaf by `double` doc values.
export class DoubleIndexSorter final : public IndexSorter {
public:
    [[nodiscard]] static Result<DoubleIndexSorter> create(std::string field, std::optional<double> missing_value,
                                                    bool reverse);

    [[nodiscard]] std::string_view provider_name() const noexcept override { return "double"; }
    [[nodiscard]] std::optional<Number> missing_value() const noexcept override;
    [[nodiscard]] Result<std::unique_ptr<DocComparator>> doc_comparator(LeafReader &leaf_reader,
                                                                       int32_t max_doc) const override;
    [[nodiscard]] Result<std::unique_ptr<ComparableProvider>> comparable_provider(
            LeafReader &leaf_reader) const override;

    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    [[nodiscard]] bool reverse() const noexcept { return reverse_mul_ < 0; }

private:
    DoubleIndexSorter(std::string field, std::optional<double> missing_value, int32_t reverse_mul);

    std::string field_;
    std::optional<double> missing_value_;
    int32_t reverse_mul_{1};
};

/// Sorts the documents of a leaf by `SortedDocValues` terms.
export class StringIndexSorter final : public IndexSorter {
public:
    /// Documents without the field sort first unless `missing_last`.
    [[nodiscard]] static Result<StringIndexSorter> create(std::string field, bool missing_last, bool reverse);

    [[nodiscard]] std::string_view provider_name() const noexcept override { return "string"; }
    [[nodiscard]] std::optional<Number> missing_value() const noexcept override { return std::nullopt; }
    [[nodiscard]] Result<std::unique_ptr<DocComparator>> doc_comparator(LeafReader &leaf_reader,
                                                                       int32_t max_doc) const override;

    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    [[nodiscard]] bool missing_last() const noexcept { return missing_last_; }
    [[nodiscard]] bool reverse() const noexcept { return reverse_mul_ < 0; }

private:
    StringIndexSorter(std::string field, bool missing_last, int32_t reverse_mul);

    std::string field_;
    bool missing_last_{false};
    int32_t reverse_mul_{1};
};

/// The permutation that sorts the `max_doc` documents of `leaf_reader`, or
/// `nullopt` when they are already in order.
///
/// Port of `Sorter::sort` (`core/index/sorter.rs`).
[[nodiscard]] Result<std::optional<IndexSortDocMapImpl>> sort_documents(const IndexSorter &sorter, LeafReader &leaf_reader,
                                                              int32_t max_doc);

/// The same, for a caller that already built the comparator — which is what an
/// index sort over several fields does, by chaining them with
/// [`ChainedDocComparator`].
[[nodiscard]] Result<std::optional<IndexSortDocMapImpl>> sort_documents_with_comparator(
        const DocComparator &comparator, int32_t max_doc);

/// Whether the permutations of `doc_map` are inverses of each other.
///
/// Port of `Sorter::is_consistent` (`core/index/sorter.rs`).
[[nodiscard]] bool is_index_sort_doc_map_consistent(const IndexSortDocMap &doc_map);

} // namespace lucene
