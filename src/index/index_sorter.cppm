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

/// The old-to-new and new-to-old document id permutations of a sorted segment.
///
/// Port of the `DocMap` trait (`core/index/index_sorter.rs`).
export class DocMap {
public:
    virtual ~DocMap() = default;

    /// How many documents the map covers.
    [[nodiscard]] virtual int32_t size() const noexcept = 0;

    /// The position `old_doc` takes in the sorted segment.
    [[nodiscard]] virtual Result<int32_t> old_to_new(int32_t old_doc) const = 0;

    /// The document that sorts into position `new_doc`.
    [[nodiscard]] virtual Result<int32_t> new_to_old(int32_t new_doc) const = 0;
};

/// A [`DocMap`] holding both permutations as plain arrays.
///
/// Port of `DocMapImpl` (`core/index/index_sorter.rs`).
///
/// **Deviation:** the reference implementation stores the permutations in
/// monotonic packed long buffers to save memory when the segment is nearly
/// sorted; the port keeps two `int32_t` arrays, which is simpler and bounded by
/// the segment size.
export class DocMapImpl final : public DocMap {
public:
    /// `new_to_old` lists the documents in their new order.
    explicit DocMapImpl(std::vector<int32_t> new_to_old);

    [[nodiscard]] int32_t size() const noexcept override;
    [[nodiscard]] Result<int32_t> old_to_new(int32_t old_doc) const override;
    [[nodiscard]] Result<int32_t> new_to_old(int32_t new_doc) const override;

    [[nodiscard]] const std::vector<int32_t> &new_to_old_values() const noexcept { return new_to_old_; }
    [[nodiscard]] const std::vector<int32_t> &old_to_new_values() const noexcept { return old_to_new_; }

private:
    std::vector<int32_t> new_to_old_;
    std::vector<int32_t> old_to_new_;
};

/// Decides how the documents of a segment should be sorted.
///
/// Port of the `IndexSorter` trait (`core/index/index_sorter.rs`).
///
/// **Deviation:** the reference implementation can also produce cross-segment
/// `ComparableProvider`s (through an `OrdinalMap`) which the merge path uses to
/// compare documents of different segments; the port only implements the
/// single-segment `doc_comparator` used when a segment is flushed, so
/// `comparable_provider` is left to a later round along with the merge wiring.
export class IndexSorter {
public:
    virtual ~IndexSorter() = default;

    /// The name that identifies this sorter when the sort is written to the
    /// segment metadata.
    [[nodiscard]] virtual std::string_view provider_name() const noexcept = 0;

    /// The value a document without the field sorts as, when the sorter has a
    /// numeric one.
    [[nodiscard]] virtual std::optional<Number> missing_value() const noexcept = 0;

    /// The comparator over the documents of `leaf_reader`.
    [[nodiscard]] virtual Result<std::unique_ptr<DocComparator>> doc_comparator(LeafReader &leaf_reader,
                                                                               int32_t max_doc) const = 0;

    /// `${provider_name}:${field}`, with `!` appended when the order is reversed.
    [[nodiscard]] std::string to_string() const;
};

/// Sorts the documents of a leaf by `int` doc values.
export class IntSorter final : public IndexSorter {
public:
    [[nodiscard]] static Result<IntSorter> create(std::string field, std::optional<int32_t> missing_value,
                                                 bool reverse);

    [[nodiscard]] std::string_view provider_name() const noexcept override { return "int"; }
    [[nodiscard]] std::optional<Number> missing_value() const noexcept override;
    [[nodiscard]] Result<std::unique_ptr<DocComparator>> doc_comparator(LeafReader &leaf_reader,
                                                                       int32_t max_doc) const override;

    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    [[nodiscard]] bool reverse() const noexcept { return reverse_mul_ < 0; }

private:
    IntSorter(std::string field, std::optional<int32_t> missing_value, int32_t reverse_mul);

    std::string field_;
    std::optional<int32_t> missing_value_;
    int32_t reverse_mul_{1};
};

/// Sorts the documents of a leaf by `long` doc values.
export class LongSorter final : public IndexSorter {
public:
    [[nodiscard]] static Result<LongSorter> create(std::string field, std::optional<int64_t> missing_value,
                                                  bool reverse);

    [[nodiscard]] std::string_view provider_name() const noexcept override { return "long"; }
    [[nodiscard]] std::optional<Number> missing_value() const noexcept override;
    [[nodiscard]] Result<std::unique_ptr<DocComparator>> doc_comparator(LeafReader &leaf_reader,
                                                                       int32_t max_doc) const override;

    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    [[nodiscard]] bool reverse() const noexcept { return reverse_mul_ < 0; }

private:
    LongSorter(std::string field, std::optional<int64_t> missing_value, int32_t reverse_mul);

    std::string field_;
    std::optional<int64_t> missing_value_;
    int32_t reverse_mul_{1};
};

/// Sorts the documents of a leaf by `float` doc values (raw bits, as the float
/// doc values field stores them).
export class FloatSorter final : public IndexSorter {
public:
    [[nodiscard]] static Result<FloatSorter> create(std::string field, std::optional<float> missing_value,
                                                   bool reverse);

    [[nodiscard]] std::string_view provider_name() const noexcept override { return "float"; }
    [[nodiscard]] std::optional<Number> missing_value() const noexcept override;
    [[nodiscard]] Result<std::unique_ptr<DocComparator>> doc_comparator(LeafReader &leaf_reader,
                                                                       int32_t max_doc) const override;

    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    [[nodiscard]] bool reverse() const noexcept { return reverse_mul_ < 0; }

private:
    FloatSorter(std::string field, std::optional<float> missing_value, int32_t reverse_mul);

    std::string field_;
    std::optional<float> missing_value_;
    int32_t reverse_mul_{1};
};

/// Sorts the documents of a leaf by `double` doc values.
export class DoubleSorter final : public IndexSorter {
public:
    [[nodiscard]] static Result<DoubleSorter> create(std::string field, std::optional<double> missing_value,
                                                    bool reverse);

    [[nodiscard]] std::string_view provider_name() const noexcept override { return "double"; }
    [[nodiscard]] std::optional<Number> missing_value() const noexcept override;
    [[nodiscard]] Result<std::unique_ptr<DocComparator>> doc_comparator(LeafReader &leaf_reader,
                                                                       int32_t max_doc) const override;

    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    [[nodiscard]] bool reverse() const noexcept { return reverse_mul_ < 0; }

private:
    DoubleSorter(std::string field, std::optional<double> missing_value, int32_t reverse_mul);

    std::string field_;
    std::optional<double> missing_value_;
    int32_t reverse_mul_{1};
};

/// Sorts the documents of a leaf by `SortedDocValues` terms.
export class StringSorter final : public IndexSorter {
public:
    /// Documents without the field sort first unless `missing_last`.
    [[nodiscard]] static Result<StringSorter> create(std::string field, bool missing_last, bool reverse);

    [[nodiscard]] std::string_view provider_name() const noexcept override { return "string"; }
    [[nodiscard]] std::optional<Number> missing_value() const noexcept override { return std::nullopt; }
    [[nodiscard]] Result<std::unique_ptr<DocComparator>> doc_comparator(LeafReader &leaf_reader,
                                                                       int32_t max_doc) const override;

    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    [[nodiscard]] bool missing_last() const noexcept { return missing_last_; }
    [[nodiscard]] bool reverse() const noexcept { return reverse_mul_ < 0; }

private:
    StringSorter(std::string field, bool missing_last, int32_t reverse_mul);

    std::string field_;
    bool missing_last_{false};
    int32_t reverse_mul_{1};
};

/// The permutation that sorts the `max_doc` documents of `leaf_reader`, or
/// `nullopt` when they are already in order.
///
/// Port of `Sorter::sort` (`core/index/sorter.rs`).
[[nodiscard]] Result<std::optional<DocMapImpl>> sort_documents(const IndexSorter &sorter, LeafReader &leaf_reader,
                                                              int32_t max_doc);

/// Whether the permutations of `doc_map` are inverses of each other.
///
/// Port of `Sorter::is_consistent` (`core/index/sorter.rs`).
[[nodiscard]] bool is_doc_map_consistent(const DocMap &doc_map);

} // namespace lucene
