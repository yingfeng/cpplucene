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

module lucene_core:index_sorter.impl;

import :index_sorter;

import std;

import :core_helper;
import :doc_id_set_iterator;
import :error;
import :leaf_reader;
import :number;
import :numeric_doc_values;
import :numeric_utils;
import :sorted_doc_values;
import :types;

namespace lucene {

namespace {

/// Runs `compare` over every adjacent pair to detect an already sorted leaf.
[[nodiscard]] bool is_sorted(const DocComparator &comparator, int32_t max_doc) {
    for (int32_t doc = 1; doc < max_doc; ++doc) {
        if (comparator.compare(doc - 1, doc) > 0) {
            return false;
        }
    }
    return true;
}

/// Orders the documents, breaking ties by document id (the reference
/// implementation's TimSort is a stable sort as well).
[[nodiscard]] std::vector<int32_t> sort_permutation(const DocComparator &comparator, int32_t max_doc) {
    std::vector<int32_t> docs(static_cast<size_t>(max_doc));
    std::iota(docs.begin(), docs.end(), int32_t{0});
    std::ranges::stable_sort(docs, [&comparator](int32_t left, int32_t right) {
        return comparator.compare(left, right) < 0;
    });
    return docs;
}

/// A comparator over an array of `long` values.
class LongArrayComparator final : public DocComparator {
public:
    LongArrayComparator(std::vector<int64_t> values, int32_t reverse_mul)
            : values_(std::move(values)), reverse_mul_(reverse_mul) {}

    [[nodiscard]] int32_t compare(int32_t doc_id1, int32_t doc_id2) const override {
        if (values_[static_cast<size_t>(doc_id1)] < values_[static_cast<size_t>(doc_id2)]) {
            return -reverse_mul_;
        }
        if (values_[static_cast<size_t>(doc_id1)] > values_[static_cast<size_t>(doc_id2)]) {
            return reverse_mul_;
        }
        return 0;
    }

private:
    std::vector<int64_t> values_;
    int32_t reverse_mul_;
};

/// A comparator over an array of `double` values; `float` values are widened on
/// the way in so both share one implementation.
class DoubleArrayComparator final : public DocComparator {
public:
    DoubleArrayComparator(std::vector<double> values, int32_t reverse_mul)
            : values_(std::move(values)), reverse_mul_(reverse_mul) {}

    [[nodiscard]] int32_t compare(int32_t doc_id1, int32_t doc_id2) const override {
        return reverse_mul_ *
               CoreHelper::compare_f64(values_[static_cast<size_t>(doc_id1)], values_[static_cast<size_t>(doc_id2)]);
    }

private:
    std::vector<double> values_;
    int32_t reverse_mul_;
};

/// A comparator over an array of term ordinals.
class OrdArrayComparator final : public DocComparator {
public:
    OrdArrayComparator(std::vector<int32_t> ords, int32_t reverse_mul)
            : ords_(std::move(ords)), reverse_mul_(reverse_mul) {}

    [[nodiscard]] int32_t compare(int32_t doc_id1, int32_t doc_id2) const override {
        const int32_t left = ords_[static_cast<size_t>(doc_id1)];
        const int32_t right = ords_[static_cast<size_t>(doc_id2)];
        if (left < right) {
            return -reverse_mul_;
        }
        if (left > right) {
            return reverse_mul_;
        }
        return 0;
    }

private:
    std::vector<int32_t> ords_;
    int32_t reverse_mul_;
};

/// The sort key of one document, read from numeric doc values and widened to a
/// `long` that sorts like the original value.
class NumericComparableProvider final : public ComparableProvider {
public:
    /// How the stored `long` turns into a sort key.
    enum class Kind : uint8_t {
        /// The value is the key.
        kLong,
        /// The value holds raw `float` bits; the key is the sortable int form.
        kFloat,
        /// The value holds `double` bits; the key is the sortable long form.
        kDouble,
    };

    NumericComparableProvider(LeafReader &leaf_reader, std::string_view field, Kind kind, int64_t missing_value) {
        auto values = leaf_reader.get_numeric_doc_values(field);
        if (values.has_value() && values.value() != nullptr) {
            values_ = std::move(values.value());
        }
        kind_ = kind;
        missing_value_ = missing_value;
    }

    [[nodiscard]] Result<int64_t> get_as_comparable_long(int32_t doc_id) override {
        int64_t value = missing_value_;
        if (values_ != nullptr) {
            auto exact = values_->advance_exact(doc_id);
            if (!exact.has_value()) {
                return std::unexpected(std::move(exact.error()));
            }
            if (exact.value()) {
                auto current = values_->long_value();
                if (!current.has_value()) {
                    return std::unexpected(std::move(current.error()));
                }
                value = current.value();
            }
        }
        switch (kind_) {
            case Kind::kLong:
                return value;
            case Kind::kFloat:
                return static_cast<int64_t>(NumericUtils::sortable_float_bits(static_cast<int32_t>(value)));
            case Kind::kDouble:
                return NumericUtils::double_to_sortable_long(std::bit_cast<double>(value));
        }
        return value;
    }

private:
    std::unique_ptr<NumericDocValues> values_;
    Kind kind_{Kind::kLong};
    int64_t missing_value_{0};
};

/// Walks the numeric doc values of `leaf_reader`, calling `on_value` for every
/// document that has one.
template <typename OnValue>
[[nodiscard]] VoidResult for_each_numeric_value(LeafReader &leaf_reader, std::string_view field, OnValue on_value) {
    auto values = leaf_reader.get_numeric_doc_values(field);
    if (!values.has_value()) {
        return std::unexpected(std::move(values.error()));
    }
    if (values.value() == nullptr) {
        // The field has no doc values in this segment: every document is missing.
        return {};
    }
    while (true) {
        auto doc = values.value()->next_doc();
        if (!doc.has_value()) {
            return std::unexpected(std::move(doc.error()));
        }
        if (doc.value() == NO_MORE_DOCS) {
            return {};
        }
        auto value = values.value()->long_value();
        if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
        }
        on_value(doc.value(), value.value());
    }
}

} // namespace

// ---- IndexSortDocMapImpl ----

IndexSortDocMapImpl::IndexSortDocMapImpl(std::vector<int32_t> new_to_old)
        : new_to_old_(std::move(new_to_old)), old_to_new_(new_to_old_.size(), -1) {
    // Every old document survives: it appears exactly once in the new order.
    for (size_t position = 0; position < new_to_old_.size(); ++position) {
        const int32_t old = new_to_old_[position];
        if (old >= 0 && static_cast<size_t>(old) < old_to_new_.size()) {
            old_to_new_[static_cast<size_t>(old)] = static_cast<int32_t>(position);
        }
    }
}

IndexSortDocMapImpl::IndexSortDocMapImpl(std::vector<int32_t> new_to_old, int32_t old_doc_count)
        : new_to_old_(std::move(new_to_old)),
          old_to_new_(static_cast<size_t>(std::max<int32_t>(old_doc_count, 0)), -1) {
    // The documents that do not appear were deleted; they keep the -1 marker.
    for (size_t position = 0; position < new_to_old_.size(); ++position) {
        const int32_t old = new_to_old_[position];
        if (old >= 0 && static_cast<size_t>(old) < old_to_new_.size()) {
            old_to_new_[static_cast<size_t>(old)] = static_cast<int32_t>(position);
        }
    }
}

int32_t IndexSortDocMapImpl::size() const noexcept {
    return static_cast<int32_t>(old_to_new_.size());
}

int32_t IndexSortDocMapImpl::new_doc_count() const noexcept {
    return static_cast<int32_t>(new_to_old_.size());
}

Result<int32_t> IndexSortDocMapImpl::old_to_new(int32_t old_doc) const {
    if (old_doc < 0 || static_cast<size_t>(old_doc) >= old_to_new_.size()) {
        return err(LuceneError::illegal_argument("doc id out of range: " + std::to_string(old_doc)));
    }
    return old_to_new_[static_cast<size_t>(old_doc)];
}

Result<int32_t> IndexSortDocMapImpl::new_to_old(int32_t new_doc) const {
    if (new_doc < 0 || static_cast<size_t>(new_doc) >= new_to_old_.size()) {
        return err(LuceneError::illegal_argument("doc id out of range: " + std::to_string(new_doc)));
    }
    return new_to_old_[static_cast<size_t>(new_doc)];
}

// ---- IndexSorter ----

std::string IndexSorter::to_string() const {
    std::string text(provider_name());
    if (auto missing = missing_value(); missing.has_value()) {
        text += " missing=" + number_to_string(missing.value());
    }
    return text;
}

Result<std::unique_ptr<ComparableProvider>> IndexSorter::comparable_provider(LeafReader & /*leaf_reader*/) const {
    return err(LuceneError::unsupported_operation(
            "this index sorter cannot produce cross-segment sort keys: " + to_string()));
}

// ---- IntIndexSorter ----

IntIndexSorter::IntIndexSorter(std::string field, std::optional<int32_t> missing_value, int32_t reverse_mul)
        : field_(std::move(field)), missing_value_(missing_value), reverse_mul_(reverse_mul) {}

Result<IntIndexSorter> IntIndexSorter::create(std::string field, std::optional<int32_t> missing_value, bool reverse) {
    if (field.empty()) {
        return err(LuceneError::illegal_argument("the sort field must not be empty"));
    }
    return IntIndexSorter(std::move(field), missing_value, reverse ? -1 : 1);
}

std::optional<Number> IntIndexSorter::missing_value() const noexcept {
    if (!missing_value_.has_value()) {
        return std::nullopt;
    }
    return Number{missing_value_.value()};
}

Result<std::unique_ptr<DocComparator>> IntIndexSorter::doc_comparator(LeafReader &leaf_reader, int32_t max_doc) const {
    std::vector<int64_t> values(static_cast<size_t>(max_doc), static_cast<int64_t>(missing_value_.value_or(0)));
    auto filled = for_each_numeric_value(leaf_reader, field_, [&values](int32_t doc, int64_t value) {
        values[static_cast<size_t>(doc)] = value;
    });
    if (!filled.has_value()) {
        return std::unexpected(std::move(filled.error()));
    }
    return std::unique_ptr<DocComparator>(new LongArrayComparator(std::move(values), reverse_mul_));
}

Result<std::unique_ptr<ComparableProvider>> IntIndexSorter::comparable_provider(LeafReader &leaf_reader) const {
    const int64_t missing = static_cast<int64_t>(missing_value_.value_or(0));
    return std::unique_ptr<ComparableProvider>(new NumericComparableProvider(
            leaf_reader, field_, NumericComparableProvider::Kind::kLong, missing));
}

// ---- LongIndexSorter ----

LongIndexSorter::LongIndexSorter(std::string field, std::optional<int64_t> missing_value, int32_t reverse_mul)
        : field_(std::move(field)), missing_value_(missing_value), reverse_mul_(reverse_mul) {}

Result<LongIndexSorter> LongIndexSorter::create(std::string field, std::optional<int64_t> missing_value, bool reverse) {
    if (field.empty()) {
        return err(LuceneError::illegal_argument("the sort field must not be empty"));
    }
    return LongIndexSorter(std::move(field), missing_value, reverse ? -1 : 1);
}

std::optional<Number> LongIndexSorter::missing_value() const noexcept {
    if (!missing_value_.has_value()) {
        return std::nullopt;
    }
    return Number{missing_value_.value()};
}

Result<std::unique_ptr<DocComparator>> LongIndexSorter::doc_comparator(LeafReader &leaf_reader, int32_t max_doc) const {
    std::vector<int64_t> values(static_cast<size_t>(max_doc), missing_value_.value_or(0));
    auto filled = for_each_numeric_value(leaf_reader, field_, [&values](int32_t doc, int64_t value) {
        values[static_cast<size_t>(doc)] = value;
    });
    if (!filled.has_value()) {
        return std::unexpected(std::move(filled.error()));
    }
    return std::unique_ptr<DocComparator>(new LongArrayComparator(std::move(values), reverse_mul_));
}

Result<std::unique_ptr<ComparableProvider>> LongIndexSorter::comparable_provider(LeafReader &leaf_reader) const {
    const int64_t missing = missing_value_.value_or(0);
    return std::unique_ptr<ComparableProvider>(new NumericComparableProvider(
            leaf_reader, field_, NumericComparableProvider::Kind::kLong, missing));
}

// ---- FloatIndexSorter ----

FloatIndexSorter::FloatIndexSorter(std::string field, std::optional<float> missing_value, int32_t reverse_mul)
        : field_(std::move(field)), missing_value_(missing_value), reverse_mul_(reverse_mul) {}

Result<FloatIndexSorter> FloatIndexSorter::create(std::string field, std::optional<float> missing_value, bool reverse) {
    if (field.empty()) {
        return err(LuceneError::illegal_argument("the sort field must not be empty"));
    }
    return FloatIndexSorter(std::move(field), missing_value, reverse ? -1 : 1);
}

std::optional<Number> FloatIndexSorter::missing_value() const noexcept {
    if (!missing_value_.has_value()) {
        return std::nullopt;
    }
    return Number{missing_value_.value()};
}

Result<std::unique_ptr<DocComparator>> FloatIndexSorter::doc_comparator(LeafReader &leaf_reader, int32_t max_doc) const {
    // The float doc values field stores the raw bits, so decode them back.
    std::vector<double> values(static_cast<size_t>(max_doc), static_cast<double>(missing_value_.value_or(0.0F)));
    auto filled = for_each_numeric_value(leaf_reader, field_, [&values](int32_t doc, int64_t value) {
        values[static_cast<size_t>(doc)] = static_cast<double>(std::bit_cast<float>(static_cast<uint32_t>(value)));
    });
    if (!filled.has_value()) {
        return std::unexpected(std::move(filled.error()));
    }
    return std::unique_ptr<DocComparator>(new DoubleArrayComparator(std::move(values), reverse_mul_));
}

Result<std::unique_ptr<ComparableProvider>> FloatIndexSorter::comparable_provider(LeafReader &leaf_reader) const {
    const int64_t missing = static_cast<int64_t>(std::bit_cast<int32_t>(missing_value_.value_or(0.0F)));
    return std::unique_ptr<ComparableProvider>(new NumericComparableProvider(
            leaf_reader, field_, NumericComparableProvider::Kind::kFloat, missing));
}

// ---- DoubleIndexSorter ----

DoubleIndexSorter::DoubleIndexSorter(std::string field, std::optional<double> missing_value, int32_t reverse_mul)
        : field_(std::move(field)), missing_value_(missing_value), reverse_mul_(reverse_mul) {}

Result<DoubleIndexSorter> DoubleIndexSorter::create(std::string field, std::optional<double> missing_value, bool reverse) {
    if (field.empty()) {
        return err(LuceneError::illegal_argument("the sort field must not be empty"));
    }
    return DoubleIndexSorter(std::move(field), missing_value, reverse ? -1 : 1);
}

std::optional<Number> DoubleIndexSorter::missing_value() const noexcept {
    if (!missing_value_.has_value()) {
        return std::nullopt;
    }
    return Number{missing_value_.value()};
}

Result<std::unique_ptr<DocComparator>> DoubleIndexSorter::doc_comparator(LeafReader &leaf_reader, int32_t max_doc) const {
    std::vector<double> values(static_cast<size_t>(max_doc), missing_value_.value_or(0.0));
    auto filled = for_each_numeric_value(leaf_reader, field_, [&values](int32_t doc, int64_t value) {
        values[static_cast<size_t>(doc)] = std::bit_cast<double>(value);
    });
    if (!filled.has_value()) {
        return std::unexpected(std::move(filled.error()));
    }
    return std::unique_ptr<DocComparator>(new DoubleArrayComparator(std::move(values), reverse_mul_));
}

Result<std::unique_ptr<ComparableProvider>> DoubleIndexSorter::comparable_provider(LeafReader &leaf_reader) const {
    const int64_t missing = std::bit_cast<int64_t>(missing_value_.value_or(0.0));
    return std::unique_ptr<ComparableProvider>(new NumericComparableProvider(
            leaf_reader, field_, NumericComparableProvider::Kind::kDouble, missing));
}

// ---- StringIndexSorter ----

StringIndexSorter::StringIndexSorter(std::string field, bool missing_last, int32_t reverse_mul)
        : field_(std::move(field)), missing_last_(missing_last), reverse_mul_(reverse_mul) {}

Result<StringIndexSorter> StringIndexSorter::create(std::string field, bool missing_last, bool reverse) {
    if (field.empty()) {
        return err(LuceneError::illegal_argument("the sort field must not be empty"));
    }
    return StringIndexSorter(std::move(field), missing_last, reverse ? -1 : 1);
}

Result<std::unique_ptr<DocComparator>> StringIndexSorter::doc_comparator(LeafReader &leaf_reader, int32_t max_doc) const {
    // A document without a term gets the smallest or the largest possible ordinal.
    const int32_t missing_ord = missing_last_ ? std::numeric_limits<int32_t>::max()
                                              : std::numeric_limits<int32_t>::min();
    std::vector<int32_t> ords(static_cast<size_t>(max_doc), missing_ord);

    auto values = leaf_reader.get_sorted_doc_values(field_);
    if (!values.has_value()) {
        return std::unexpected(std::move(values.error()));
    }
    if (values.value() != nullptr) {
        while (true) {
            auto doc = values.value()->next_doc();
            if (!doc.has_value()) {
                return std::unexpected(std::move(doc.error()));
            }
            if (doc.value() == NO_MORE_DOCS) {
                break;
            }
            auto ord = values.value()->ord_value();
            if (!ord.has_value()) {
                return std::unexpected(std::move(ord.error()));
            }
            ords[static_cast<size_t>(doc.value())] = ord.value();
        }
    }
    return std::unique_ptr<DocComparator>(new OrdArrayComparator(std::move(ords), reverse_mul_));
}

// ---- sorting ----

Result<std::optional<IndexSortDocMapImpl>> sort_documents(const IndexSorter &sorter, LeafReader &leaf_reader, int32_t max_doc) {
    if (max_doc < 0) {
        return err(LuceneError::illegal_argument("max_doc must not be negative"));
    }
    auto comparator = sorter.doc_comparator(leaf_reader, max_doc);
    if (!comparator.has_value()) {
        return std::unexpected(std::move(comparator.error()));
    }
    return sort_documents_with_comparator(*comparator.value(), max_doc);
}

Result<std::optional<IndexSortDocMapImpl>> sort_documents_with_comparator(const DocComparator &comparator,
                                                                          int32_t max_doc) {
    if (max_doc < 0) {
        return err(LuceneError::illegal_argument("max_doc must not be negative"));
    }
    if (max_doc <= 1 || is_sorted(comparator, max_doc)) {
        return std::optional<IndexSortDocMapImpl>{};
    }
    return std::optional<IndexSortDocMapImpl>{std::in_place, sort_permutation(comparator, max_doc)};
}

ChainedDocComparator::ChainedDocComparator(std::vector<std::unique_ptr<DocComparator>> comparators)
        : comparators_(std::move(comparators)) {}

Result<std::unique_ptr<ChainedDocComparator>> ChainedDocComparator::create(
        std::vector<std::unique_ptr<DocComparator>> comparators) {
    if (comparators.empty()) {
        return err(LuceneError::illegal_argument("a chained comparator needs at least one comparator"));
    }
    for (const auto &comparator : comparators) {
        if (comparator == nullptr) {
            return err(LuceneError::illegal_argument("a chained comparator must not be null"));
        }
    }
    return std::unique_ptr<ChainedDocComparator>(new ChainedDocComparator(std::move(comparators)));
}

int32_t ChainedDocComparator::compare(int32_t doc_id1, int32_t doc_id2) const {
    for (const auto &comparator : comparators_) {
        const int32_t result = comparator->compare(doc_id1, doc_id2);
        if (result != 0) {
            return result;
        }
    }
    return 0;
}

bool is_index_sort_doc_map_consistent(const IndexSortDocMap &doc_map) {
    const int32_t max_doc = doc_map.size();
    for (int32_t doc = 0; doc < max_doc; ++doc) {
        auto new_doc = doc_map.old_to_new(doc);
        if (!new_doc.has_value() || new_doc.value() < 0 || new_doc.value() >= max_doc) {
            return false;
        }
        auto old_doc = doc_map.new_to_old(new_doc.value());
        if (!old_doc.has_value() || old_doc.value() != doc) {
            return false;
        }
    }
    return true;
}

} // namespace lucene
