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

export module lucene_core:sorting_doc_values;

import :types;
import std;

import :binary_doc_values;
import :error;
import :index_sorter;
import :numeric_doc_values;
import :sorted_doc_values;
import :sorted_numeric_doc_values;
import :sorted_set_doc_values;

namespace lucene {

/// The doc values of a segment, presented in index-sorted order.
///
/// Port of the doc values half of `sorting_codec_reader.rs`
/// (`core/index/sorting_codec_reader.rs`).
///
/// A segment written before the index sort was applied has its documents in
/// insertion order; the sorting wrappers below renumber them so that document `N`
/// of the wrapper is document `new_to_old(N)` of the segment.
///
/// **Deviation:** the reference implementation keeps the underlying iterator and
/// maps the doc ids as it goes; the port materializes the values of the whole
/// leaf once (an O(max_doc) array, like the reference's own `doc_to_ord` table),
/// which is simpler and lets `advance_exact` run in new-doc order even though the
/// underlying iterators are forward-only over the old order.
export class SortingNumericDocValues final : public NumericDocValues {
public:
    [[nodiscard]] static Result<std::unique_ptr<SortingNumericDocValues>> create(
            std::unique_ptr<NumericDocValues> values, const IndexSortDocMap &doc_map);

    [[nodiscard]] int32_t doc_id() const override;
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override;
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override;
    [[nodiscard]] Result<int64_t> long_value() override;

private:
    SortingNumericDocValues(std::vector<int32_t> new_docs, std::vector<int64_t> values);

    std::vector<int32_t> new_docs_;
    std::vector<int64_t> values_;
    size_t position_{0};
    int32_t doc_{-1};
};

/// [`SortingNumericDocValues`] for a binary field.
export class SortingBinaryDocValues final : public BinaryDocValues {
public:
    [[nodiscard]] static Result<std::unique_ptr<SortingBinaryDocValues>> create(
            std::unique_ptr<BinaryDocValues> values, const IndexSortDocMap &doc_map);

    [[nodiscard]] int32_t doc_id() const override;
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override;
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override;
    [[nodiscard]] Result<BytesRef> binary_value() override;

private:
    SortingBinaryDocValues(std::vector<int32_t> new_docs, std::vector<BytesRef> values);

    std::vector<int32_t> new_docs_;
    std::vector<BytesRef> values_;
    size_t position_{0};
    int32_t doc_{-1};
};

/// [`SortingNumericDocValues`] for a sorted (term) field; the ordinals refer to
/// the underlying segment's dictionary, so lookups delegate to it.
export class SortingSortedDocValues final : public SortedDocValues {
public:
    [[nodiscard]] static Result<std::unique_ptr<SortingSortedDocValues>> create(
            std::unique_ptr<SortedDocValues> values, const IndexSortDocMap &doc_map);

    [[nodiscard]] int32_t doc_id() const override;
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override;
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override;
    [[nodiscard]] Result<int32_t> ord_value() override;
    [[nodiscard]] Result<BytesRef> lookup_ord(int32_t ord) override;
    [[nodiscard]] Result<int32_t> get_value_count() const override;

private:
    SortingSortedDocValues(std::vector<int32_t> new_docs, std::vector<int32_t> ords,
                           std::unique_ptr<SortedDocValues> values);

    std::vector<int32_t> new_docs_;
    std::vector<int32_t> ords_;
    std::unique_ptr<SortedDocValues> values_;
    size_t position_{0};
    int32_t doc_{-1};
};

/// [`SortingNumericDocValues`] for a multi-valued numeric field.
export class SortingSortedNumericDocValues final : public SortedNumericDocValues {
public:
    [[nodiscard]] static Result<std::unique_ptr<SortingSortedNumericDocValues>> create(
            std::unique_ptr<SortedNumericDocValues> values, const IndexSortDocMap &doc_map);

    [[nodiscard]] int32_t doc_id() const override;
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override;
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override;
    [[nodiscard]] Result<int64_t> next_value() override;
    [[nodiscard]] Result<int32_t> doc_value_count() override;

private:
    SortingSortedNumericDocValues(std::vector<int32_t> new_docs, std::vector<std::vector<int64_t>> values);

    std::vector<int32_t> new_docs_;
    std::vector<std::vector<int64_t>> values_;
    size_t position_{0};
    size_t value_position_{0};
    int32_t doc_{-1};
};

/// [`SortingNumericDocValues`] for a multi-valued term field.
export class SortingSortedSetDocValues final : public SortedSetDocValues {
public:
    [[nodiscard]] static Result<std::unique_ptr<SortingSortedSetDocValues>> create(
            std::unique_ptr<SortedSetDocValues> values, const IndexSortDocMap &doc_map);

    [[nodiscard]] int32_t doc_id() const override;
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override;
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override;
    [[nodiscard]] Result<int64_t> next_ord() override;
    [[nodiscard]] Result<int32_t> doc_value_count() override;
    [[nodiscard]] Result<BytesRef> lookup_ord(int64_t ord) override;
    [[nodiscard]] Result<int64_t> get_value_count() const override;

private:
    SortingSortedSetDocValues(std::vector<int32_t> new_docs, std::vector<std::vector<int64_t>> ords,
                              std::unique_ptr<SortedSetDocValues> values);

    std::vector<int32_t> new_docs_;
    std::vector<std::vector<int64_t>> ords_;
    std::unique_ptr<SortedSetDocValues> values_;
    size_t position_{0};
    size_t ord_position_{0};
    int32_t doc_{-1};
};

/// The `new doc -> old doc` pairs of `doc_map` for the documents that have a
/// value, sorted by the new doc id.
///
/// `next_doc` is the only traversal an underlying doc values iterator supports,
/// so every wrapper starts from this table.
[[nodiscard]] Result<std::vector<std::pair<int32_t, int32_t>>> new_doc_to_old(DocIdSetIterator &values,
                                                                             const IndexSortDocMap &doc_map);

} // namespace lucene
