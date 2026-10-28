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

export module lucene_core:field_comparator;

import :types;
import std;

import :error;
import :leaf_reader_context;
import :scorable;
import :sort_field;

namespace lucene {

/// The value of a sort field for a document that has no value in it.
///
/// Port of the `Missing` variant of `FieldComparatorValue`
/// (`core/search/field_comparator.rs`): a missing value sorts after every real
/// value, so it is kept as a tag rather than as a sentinel number.
export struct SortValueMissing {
    friend bool operator==(const SortValueMissing &, const SortValueMissing &) = default;
};

/// One sort value of a hit: a document number, an ordinal, a score or a numeric
/// value, a term, or a missing value.
export using SortValue = std::variant<SortValueMissing, int32_t, int64_t, float, double, std::string>;

/// Compares the values of one sort field across the hits of one leaf, and across
/// leaves through the values collected so far.
///
/// Port of the `FieldComparator` trait (`core/search/field_comparator.rs`).
///
/// **Deviation:** the reference implementation keeps the natural order and lets
/// the hit queue apply [`reverse`](SortField::reverse); here every comparator owns
/// its reverse flag and already inverts its results, so callers always get "sorts
/// first is negative".
export class FieldComparator {
public:
    virtual ~FieldComparator() = default;

    /// Compares the values of two collected slots.
    [[nodiscard]] virtual int32_t compare(int32_t slot1, int32_t slot2) const = 0;

    /// Compares the value of `slot` with the current bottom of the queue.
    [[nodiscard]] virtual int32_t compare_bottom(int32_t slot) const = 0;

    /// Compares the value of `slot` with the top value, or `0` when no top value
    /// was set.
    [[nodiscard]] virtual int32_t compare_top(int32_t slot) const = 0;

    /// Records the value of `slot` as the new bottom of the queue.
    virtual void set_bottom(int32_t slot) = 0;

    /// The value a paged search starts after, as reported by [`value`] of the
    /// same comparator; a value of the wrong type is rejected, because a page
    /// positioned by a value of another type would silently skip hits.
    [[nodiscard]] virtual VoidResult set_top_value(const SortValue &value) = 0;

    /// The value collected in `slot`.
    [[nodiscard]] virtual Result<SortValue> value(int32_t slot) const = 0;

    /// A comparator over one leaf.
    [[nodiscard]] virtual Result<std::unique_ptr<class LeafFieldComparator>>
    get_leaf_comparator(const LeafReaderContext &context) = 0;

    /// Compares two exposed values, for merging the results of several shards.
    [[nodiscard]] virtual Result<int32_t> compare_values(const SortValue &first, const SortValue &second) const = 0;
};

/// Compares the values of one leaf; the collected slots, the bottom and the top
/// value all live in the [`FieldComparator`] it came from, which is why a leaf
/// does not have its own `set_top_value`.
///
/// Port of the `LeafFieldComparator` trait (`core/search/leaf_field_comparator.rs`).
export class LeafFieldComparator {
public:
    virtual ~LeafFieldComparator() = default;

    /// The scorer of the current leaf, for the comparators that sort on scores.
    virtual void set_scorer(Scorable & /*scorer*/) {}

    /// Records the value of `slot` as the bottom of the queue.
    virtual void set_bottom(int32_t slot) = 0;

    /// `> 0` when the value of `doc` sorts after the bottom, `< 0` when before.
    [[nodiscard]] virtual Result<int32_t> compare_bottom(int32_t doc) = 0;

    /// `< 0` when the value of `doc` sorts before the top value, `> 0` when
    /// after, `0` when it ties it.
    [[nodiscard]] virtual Result<int32_t> compare_top(int32_t doc) = 0;

    /// Copies the value of `doc` into `slot`.
    [[nodiscard]] virtual VoidResult copy(int32_t slot, int32_t doc) = 0;

    /// Tells the comparator that the hit count reached the total-hits threshold.
    /// Only the first comparator of a sort is told.
    ///
    /// Port of `LeafFieldComparator::set_hits_threshold_reached`
    /// (`core/search/leaf_field_comparator.rs`).
    ///
    /// **Deviation:** every comparator ignores it.  In the reference
    /// implementation it only drives the competitive iterators (which skip
    /// documents that can no longer win), and the port has none of those; the
    /// score comparator's implementation there is the default no-op as well.
    virtual void set_hits_threshold_reached() {}
};

/// Sorts a leaf by relevance.
///
/// Port of `RelevanceComparator` (`core/search/field_comparator.rs`).
export class ScoreFieldComparator final : public FieldComparator {
public:
    /// A comparator of `num_hits` slots, highest score first.
    explicit ScoreFieldComparator(int32_t num_hits, bool reverse = false);

    [[nodiscard]] int32_t compare(int32_t slot1, int32_t slot2) const override;
    [[nodiscard]] int32_t compare_bottom(int32_t slot) const override;
    [[nodiscard]] int32_t compare_top(int32_t slot) const override;
    void set_bottom(int32_t slot) override;
    [[nodiscard]] VoidResult set_top_value(const SortValue &value) override;
    [[nodiscard]] Result<SortValue> value(int32_t slot) const override;
    [[nodiscard]] Result<std::unique_ptr<LeafFieldComparator>>
    get_leaf_comparator(const LeafReaderContext &context) override;
    [[nodiscard]] Result<int32_t> compare_values(const SortValue &first, const SortValue &second) const override;

private:
    std::vector<float> values_;
    float bottom_{0.0F};
    std::optional<float> top_{};
    bool reverse_{false};
};

/// Sorts a leaf by document number.
///
/// Port of `DocComparator` (`core/search/comparators/doc_comparator.rs`).
export class DocFieldComparator final : public FieldComparator {
public:
    /// A comparator of `num_hits` slots, lowest document first.
    explicit DocFieldComparator(int32_t num_hits, bool reverse = false);

    [[nodiscard]] int32_t compare(int32_t slot1, int32_t slot2) const override;
    [[nodiscard]] int32_t compare_bottom(int32_t slot) const override;
    [[nodiscard]] int32_t compare_top(int32_t slot) const override;
    void set_bottom(int32_t slot) override;
    [[nodiscard]] VoidResult set_top_value(const SortValue &value) override;
    [[nodiscard]] Result<SortValue> value(int32_t slot) const override;
    [[nodiscard]] Result<std::unique_ptr<LeafFieldComparator>>
    get_leaf_comparator(const LeafReaderContext &context) override;
    [[nodiscard]] Result<int32_t> compare_values(const SortValue &first, const SortValue &second) const override;

private:
    std::vector<int32_t> values_;
    int32_t bottom_{0};
    std::optional<int32_t> top_{};
    bool reverse_{false};
};

/// Sorts a leaf by a doc values field: a term for
/// [`kString`](SortFieldType::kString), a number for the numeric types.
///
/// Ports the `TermOrdValComparator` and the `Int`/`Long`/`Float`/`Double`
/// comparators (`core/search/comparators/*`) as one class, because they differ
/// only in how a value is read.
///
/// **Deviations:**
///  * terms are compared as strings rather than through global ordinals, which
///    gives the same order without the per-segment ordinal map;
///  * a document without a value sorts as the empty string or as `0`;
///  * the multi-valued types pick the smallest or the largest value of a document
///    ([`SortFieldSelector`]), which is what the reference implementation's
///    `SortedNumericSelector`/`SortedSetSelector` do, without their wrapper
///    iterations.
export class DocValuesFieldComparator final : public FieldComparator {
public:
    /// A comparator of `num_hits` slots over `field` of `type`, reversed when
    /// `reverse`; for a multi-valued type `selector` picks the value of a
    /// document that has several.
    DocValuesFieldComparator(std::string field, SortFieldType type, bool reverse, int32_t num_hits,
                             SortFieldSelector selector = SortFieldSelector::kMin);

    [[nodiscard]] int32_t compare(int32_t slot1, int32_t slot2) const override;
    [[nodiscard]] int32_t compare_bottom(int32_t slot) const override;
    [[nodiscard]] int32_t compare_top(int32_t slot) const override;
    void set_bottom(int32_t slot) override;
    [[nodiscard]] VoidResult set_top_value(const SortValue &value) override;
    [[nodiscard]] Result<SortValue> value(int32_t slot) const override;
    [[nodiscard]] Result<std::unique_ptr<LeafFieldComparator>>
    get_leaf_comparator(const LeafReaderContext &context) override;
    [[nodiscard]] Result<int32_t> compare_values(const SortValue &first, const SortValue &second) const override;

    /// The field this comparator reads.
    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    /// How the values are read.
    [[nodiscard]] SortFieldType type() const noexcept { return type_; }
    /// Whether the order is reversed.
    [[nodiscard]] bool reverse() const noexcept { return reverse_; }
    /// Which value of a multi-valued field is used.
    [[nodiscard]] SortFieldSelector selector() const noexcept { return selector_; }

private:
    [[nodiscard]] int32_t compare_held(const SortValue &first, const SortValue &second) const;

    std::string field_;
    SortFieldType type_;
    bool reverse_{false};
    SortFieldSelector selector_{SortFieldSelector::kMin};
    /// `std::string` for [`kString`](SortFieldType::kString), `int64_t` or
    /// `double` for the numeric types.
    std::vector<SortValue> values_;
    SortValue bottom_;
    std::optional<SortValue> top_{};
};

/// The comparator a sort field needs.
[[nodiscard]] Result<std::unique_ptr<FieldComparator>> comparator_for(const SortField &sort_field, int32_t num_hits);

} // namespace lucene
