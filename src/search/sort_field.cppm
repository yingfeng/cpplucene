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

export module lucene_core:sort_field;

import :types;
import std;

import :error;

namespace lucene {

/// How the values of a sort field are read and compared.
///
/// Port of `SortFieldType` (`core/search/sort_field.rs`).
///
/// **Deviation:** the reference implementation's `StringVal`/`IntVal`/... types,
/// which sort on doc values that are not also indexed as points, and the
/// `Custom`/`Rewritable` types are not ported: every value is read from doc
/// values, which is what the port's segments write.
export enum class SortFieldType : int32_t {
    /// Sort by relevance; higher scores come first.
    kScore = 0,
    /// Sort by document number; lower documents come first.
    kDoc = 1,
    /// Sort by a `SortedDocValues` term; lower terms come first.
    kString = 2,
    /// Sort by an `int` in `NumericDocValues`.
    kInt = 3,
    /// Sort by a `float` in `NumericDocValues`.
    kFloat = 4,
    /// Sort by a `long` in `NumericDocValues`.
    kLong = 5,
    /// Sort by a `double` in `NumericDocValues`.
    kDouble = 6,
    /// Sort by a `long` in `SortedNumericDocValues`, taking the
    /// [`selector`](SortField::selector) value.
    kSortedNumeric = 7,
    /// Sort by a `double` in `SortedNumericDocValues`, taking the
    /// [`selector`](SortField::selector) value (the values hold the raw bits).
    kSortedNumericDouble = 8,
    /// Sort by a term in `SortedSetDocValues`, taking the
    /// [`selector`](SortField::selector) value.
    kSortedSet = 9,
};

/// Which value of a multi-valued field a sort uses.
export enum class SortFieldSelector : uint8_t {
    /// The smallest value of the document.
    kMin = 0,
    /// The largest value of the document.
    kMax = 1,
};

/// How one field takes part in a sort.
///
/// Port of `SortField` (`core/search/sort_field.rs`).
///
/// **Deviation:** missing values (`sortMissingFirst`/`sortMissingLast`) are not
/// ported; a document without a value sorts as `0` for a numeric field and as the
/// empty string for a term field, which is the reference implementation's default.
export class SortField {
public:
    /// A sort on `field` of `type`; the field may be absent only for the
    /// [`kScore`](SortFieldType::kScore) and [`kDoc`](SortFieldType::kDoc) types.
    [[nodiscard]] static Result<SortField> create(std::optional<std::string> field, SortFieldType type);

    /// [`create`], with the natural order reversed when `reverse`.
    [[nodiscard]] static Result<SortField> with_reverse(std::optional<std::string> field, SortFieldType type,
                                                       bool reverse);

    /// A sort on a multi-valued field; `selector` picks the value of a document
    /// that has several.  `type` has to be one of the multi-valued types.
    [[nodiscard]] static Result<SortField> with_selector(std::optional<std::string> field, SortFieldType type,
                                                         SortFieldSelector selector);

    /// [`with_selector`], with the natural order reversed when `reverse`.
    [[nodiscard]] static Result<SortField> with_reverse_selector(std::optional<std::string> field, SortFieldType type,
                                                               SortFieldSelector selector, bool reverse);

    /// Sorts by descending relevance.
    [[nodiscard]] static Result<SortField> score();

    /// Sorts by ascending document number.
    [[nodiscard]] static Result<SortField> doc();

    /// The field the sort reads, absent for the score and doc types.
    [[nodiscard]] const std::optional<std::string> &field() const noexcept { return field_; }
    /// How the values are read and compared.
    [[nodiscard]] SortFieldType type() const noexcept { return type_; }
    /// Whether the natural order is reversed.
    [[nodiscard]] bool reverse() const noexcept { return reverse_; }
    /// Which value of a multi-valued field the sort uses.
    [[nodiscard]] SortFieldSelector selector() const noexcept { return selector_; }
    /// Whether the type reads a multi-valued field.
    [[nodiscard]] bool is_multi_valued() const noexcept {
        return type_ == SortFieldType::kSortedNumeric || type_ == SortFieldType::kSortedNumericDouble ||
               type_ == SortFieldType::kSortedSet;
    }
    /// Whether sorting needs the scores, i.e. the type is
    /// [`kScore`](SortFieldType::kScore).
    [[nodiscard]] bool needs_scores() const noexcept { return type_ == SortFieldType::kScore; }

    /// `<score>`, `<string: "field">` or `<int: "field">`, with `!` appended when
    /// reversed, the form the reference implementation prints.
    [[nodiscard]] Result<std::string> to_string() const;

    [[nodiscard]] bool equals(const SortField &other) const noexcept {
        return field_ == other.field_ && type_ == other.type_ && reverse_ == other.reverse_ &&
               selector_ == other.selector_;
    }

    [[nodiscard]] size_t hash() const noexcept;

    [[nodiscard]] bool operator==(const SortField &other) const noexcept { return equals(other); }

private:
    SortField(std::optional<std::string> field, SortFieldType type, bool reverse, SortFieldSelector selector);

    std::optional<std::string> field_;
    SortFieldType type_{SortFieldType::kScore};
    bool reverse_{false};
    SortFieldSelector selector_{SortFieldSelector::kMin};
};

/// The sort criteria of a search: the fields are compared in order, and the
/// document number breaks the remaining ties.
///
/// Port of `Sort` (`core/search/sort.rs`).
export class Sort {
public:
    /// Sorts by relevance.
    [[nodiscard]] static Result<Sort> relevance();

    /// Sorts by index order.
    [[nodiscard]] static Result<Sort> index_order();

    /// Sorts by `fields` in priority order; the list must not be empty.
    [[nodiscard]] static Result<Sort> with_fields(std::vector<SortField> fields);

    /// The sort fields, in priority order.
    [[nodiscard]] const std::vector<SortField> &fields() const noexcept { return fields_; }

    /// Whether any field needs the scores.
    [[nodiscard]] bool needs_scores() const noexcept;

    /// The fields joined with `,`, the form the reference implementation prints.
    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] bool equals(const Sort &other) const noexcept { return fields_ == other.fields_; }

    [[nodiscard]] size_t hash() const noexcept;

private:
    explicit Sort(std::vector<SortField> fields);

    std::vector<SortField> fields_;
};

} // namespace lucene
