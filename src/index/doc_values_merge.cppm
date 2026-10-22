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

export module lucene_core:doc_values_merge;

import :types;
import std;

import :binary_doc_values;
import :bytes_ref;
import :doc_id_merger;
import :doc_id_set_iterator;
import :doc_map;
import :error;
import :numeric_doc_values;
import :sorted_doc_values;
import :sorted_numeric_doc_values;
import :sorted_set_doc_values;

namespace lucene {

// ---------------------------------------------------------------------------
// Streaming merges: the values fit one cursor per segment and are simply merged
// in doc-id order.
// ---------------------------------------------------------------------------

/// One input segment's numeric doc values (or norms) plus the map into the
/// merged segment.
///
/// Port of the reference implementation's `NumericDocValuesSub`.  The doc values cursor must be
/// freshly created, i.e. positioned on doc id `-1`.
export class NumericDocValuesSub {
public:
    NumericDocValuesSub(std::unique_ptr<NumericDocValues> values, const DocMap &doc_map);

    /// The raw next doc id of the sub-reader, or `NO_MORE_DOCS`.
    [[nodiscard]] Result<int32_t> next_doc();

    [[nodiscard]] const DocMap &get_doc_map() const { return *doc_map_; }

    [[nodiscard]] NumericDocValues &values() { return *values_; }

private:
    std::unique_ptr<NumericDocValues> values_;
    const DocMap *doc_map_;
};

/// Presents the numeric doc values of several segments as one, rebased onto the
/// merged segment's doc ids and skipping deleted documents.
///
/// Port of `NumericDocValuesMerge`.  This is a forward-only iterator: `advance`
/// and `advance_exact` are not supported, because merging is a pure k-way merge
/// of the sub cursors (they are always created fresh for the whole merge).
export class NumericDocValuesMerge final : public NumericDocValues {
public:
    /// Builds the merger over `subs`; `needs_index_sort` selects the sorted
    /// doc-id merger (never true in this port, which has no index sorting).
    [[nodiscard]] static Result<std::unique_ptr<NumericDocValuesMerge>> create(
            std::vector<NumericDocValuesSub> subs, bool needs_index_sort);

    [[nodiscard]] int32_t doc_id() const override { return doc_id_; }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t /*target*/) override;
    [[nodiscard]] Result<int64_t> cost() const override { return 0; }
    [[nodiscard]] Result<bool> advance_exact(int32_t /*target*/) override;
    [[nodiscard]] Result<int64_t> long_value() override;

private:
    NumericDocValuesMerge(DocIdMerger<NumericDocValuesSub> merger);

    DocIdMerger<NumericDocValuesSub> merger_;
    std::optional<size_t> current_;
    int32_t doc_id_{-1};
};

/// One input segment's binary doc values, plus the map into the merged segment.
export class BinaryDocValuesSub {
public:
    BinaryDocValuesSub(std::unique_ptr<BinaryDocValues> values, const DocMap &doc_map);

    [[nodiscard]] Result<int32_t> next_doc();
    [[nodiscard]] const DocMap &get_doc_map() const { return *doc_map_; }
    [[nodiscard]] BinaryDocValues &values() { return *values_; }

private:
    std::unique_ptr<BinaryDocValues> values_;
    const DocMap *doc_map_;
};

/// Presents the binary doc values of several segments as one.
///
/// Port of `BinaryDocValuesMerge`; same forward-only contract as
/// [`NumericDocValuesMerge`].
export class BinaryDocValuesMerge final : public BinaryDocValues {
public:
    [[nodiscard]] static Result<std::unique_ptr<BinaryDocValuesMerge>> create(std::vector<BinaryDocValuesSub> subs,
                                                                             bool needs_index_sort);

    [[nodiscard]] int32_t doc_id() const override { return doc_id_; }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t /*target*/) override;
    [[nodiscard]] Result<int64_t> cost() const override { return 0; }
    [[nodiscard]] Result<bool> advance_exact(int32_t /*target*/) override;
    [[nodiscard]] Result<BytesRef> binary_value() override;

private:
    BinaryDocValuesMerge(DocIdMerger<BinaryDocValuesSub> merger);

    DocIdMerger<BinaryDocValuesSub> merger_;
    std::optional<size_t> current_;
    int32_t doc_id_{-1};
};

// ---------------------------------------------------------------------------
// Materialised merges: sorted values need the union of the dictionaries before a
// single document can be written, so the merge buffers the result.
// ---------------------------------------------------------------------------

/// Dense numeric doc values over `values` (doc `i` has `values[i]`).
export class InMemoryNumericDocValues final : public NumericDocValues {
public:
    explicit InMemoryNumericDocValues(std::vector<int64_t> values);

    [[nodiscard]] int32_t doc_id() const override { return doc_id_; }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(values_.size()); }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override;
    [[nodiscard]] Result<int64_t> long_value() override;

private:
    std::vector<int64_t> values_;
    int32_t doc_id_{-1};
};

/// Dense binary doc values over `values` (doc `i` has `values[i]`).
export class InMemoryBinaryDocValues final : public BinaryDocValues {
public:
    explicit InMemoryBinaryDocValues(std::vector<BytesRef> values);

    [[nodiscard]] int32_t doc_id() const override { return doc_id_; }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(values_.size()); }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override;
    [[nodiscard]] Result<BytesRef> binary_value() override;

private:
    std::vector<BytesRef> values_;
    int32_t doc_id_{-1};
};

/// Sorted doc values over a precomputed ordinal array and dictionary.
///
/// `ords[doc]` is the ordinal of that document, or `-1` when it has no value;
/// `terms` is the sorted, deduplicated dictionary.
export class InMemorySortedDocValues final : public SortedDocValues {
public:
    InMemorySortedDocValues(std::vector<int32_t> ords, std::vector<BytesRef> terms);

    [[nodiscard]] int32_t doc_id() const override { return doc_id_; }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(ords_.size()); }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override;
    [[nodiscard]] Result<int32_t> ord_value() override;
    [[nodiscard]] Result<BytesRef> lookup_ord(int32_t ord) override;
    [[nodiscard]] Result<int32_t> get_value_count() const override;
    [[nodiscard]] Result<int32_t> lookup_term(std::span<const uint8_t> key) override;

private:
    std::vector<int32_t> ords_;
    std::vector<BytesRef> terms_;
    int32_t doc_id_{-1};
    int32_t next_index_{0};
};

/// Sorted set doc values over a precomputed ordinal array and dictionary.
export class InMemorySortedSetDocValues final : public SortedSetDocValues {
public:
    InMemorySortedSetDocValues(std::vector<std::vector<int64_t>> ords, std::vector<BytesRef> terms);

    [[nodiscard]] int32_t doc_id() const override { return doc_id_; }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(ords_.size()); }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override;
    [[nodiscard]] Result<int64_t> next_ord() override;
    [[nodiscard]] Result<int32_t> doc_value_count() override;
    [[nodiscard]] Result<BytesRef> lookup_ord(int64_t ord) override;
    [[nodiscard]] Result<int64_t> get_value_count() const override;
    [[nodiscard]] Result<int64_t> lookup_term(std::span<const uint8_t> key) override;

private:
    std::vector<std::vector<int64_t>> ords_;
    std::vector<BytesRef> terms_;
    int32_t doc_id_{-1};
    int32_t next_index_{0};
    size_t ord_index_{0};
};

/// Sorted numeric doc values over precomputed per-document value lists.
export class InMemorySortedNumericDocValues final : public SortedNumericDocValues {
public:
    explicit InMemorySortedNumericDocValues(std::vector<std::vector<int64_t>> values);

    [[nodiscard]] int32_t doc_id() const override { return doc_id_; }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(values_.size()); }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override;
    [[nodiscard]] Result<int64_t> next_value() override;
    [[nodiscard]] Result<int32_t> doc_value_count() override;
    [[nodiscard]] bool is_single_valued() const override { return single_valued_; }
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric_doc_values() override;

private:
    std::vector<std::vector<int64_t>> values_;
    bool single_valued_{false};
    int32_t doc_id_{-1};
    int32_t next_index_{0};
    size_t value_index_{0};
};

} // namespace lucene
