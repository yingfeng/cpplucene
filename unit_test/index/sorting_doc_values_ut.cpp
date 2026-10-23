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

module;

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

module lucene_core:ut.sorting_doc_values;

import lucene_core;

using namespace lucene;

namespace {

/// Numeric doc values over `values`; `-1` marks a document without a value.
class FakeNumericDocValues final : public NumericDocValues {
public:
    explicit FakeNumericDocValues(std::vector<int64_t> values) : values_(std::move(values)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        for (int32_t doc = doc_ < 0 ? 0 : doc_ + 1; doc < static_cast<int32_t>(values_.size()); ++doc) {
            if (values_[static_cast<size_t>(doc)] != -1) {
                doc_ = doc;
                return doc_;
            }
        }
        doc_ = NO_MORE_DOCS;
        return doc_;
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        for (int32_t doc = target; doc < static_cast<int32_t>(values_.size()); ++doc) {
            if (values_[static_cast<size_t>(doc)] != -1) {
                doc_ = doc;
                return doc_;
            }
        }
        doc_ = NO_MORE_DOCS;
        return doc_;
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(values_.size()); }

    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        if (target < 0 || static_cast<size_t>(target) >= values_.size()) {
            return false;
        }
        const int64_t value = values_[static_cast<size_t>(target)];
        if (value == -1) {
            return false;
        }
        doc_ = target;
        return true;
    }

    [[nodiscard]] Result<int64_t> long_value() override {
        if (doc_ < 0 || static_cast<size_t>(doc_) >= values_.size()) {
            return err(LuceneError::illegal_state("not positioned"));
        }
        return values_[static_cast<size_t>(doc_)];
    }

private:
    std::vector<int64_t> values_;
    int32_t doc_{-1};
};

/// Sorted doc values over `ords`; `-1` marks a document without a value, and the
/// dictionary is the list of terms.
class FakeSortedDocValues final : public SortedDocValues {
public:
    FakeSortedDocValues(std::vector<int32_t> ords, std::vector<std::string> terms)
            : ords_(std::move(ords)), terms_(std::move(terms)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        for (int32_t doc = doc_ < 0 ? 0 : doc_ + 1; doc < static_cast<int32_t>(ords_.size()); ++doc) {
            if (ords_[static_cast<size_t>(doc)] != -1) {
                doc_ = doc;
                return doc_;
            }
        }
        doc_ = NO_MORE_DOCS;
        return doc_;
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        for (int32_t doc = target; doc < static_cast<int32_t>(ords_.size()); ++doc) {
            if (ords_[static_cast<size_t>(doc)] != -1) {
                doc_ = doc;
                return doc_;
            }
        }
        doc_ = NO_MORE_DOCS;
        return doc_;
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(ords_.size()); }

    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        if (target < 0 || static_cast<size_t>(target) >= ords_.size()) {
            return false;
        }
        if (ords_[static_cast<size_t>(target)] == -1) {
            return false;
        }
        doc_ = target;
        return true;
    }

    [[nodiscard]] Result<int32_t> ord_value() override {
        if (doc_ < 0 || static_cast<size_t>(doc_) >= ords_.size()) {
            return err(LuceneError::illegal_state("not positioned"));
        }
        return ords_[static_cast<size_t>(doc_)];
    }

    [[nodiscard]] Result<BytesRef> lookup_ord(int32_t ord) override {
        if (ord < 0 || static_cast<size_t>(ord) >= terms_.size()) {
            return err(LuceneError::illegal_argument("ordinal out of range"));
        }
        return BytesRef::from_string(terms_[static_cast<size_t>(ord)]);
    }

    [[nodiscard]] Result<int32_t> get_value_count() const override { return static_cast<int32_t>(terms_.size()); }

private:
    std::vector<int32_t> ords_;
    std::vector<std::string> terms_;
    int32_t doc_{-1};
};

/// Drains a doc values iterator, returning `doc:value` pairs in iterator order.
template <typename Values, typename ValueOf>
std::vector<std::pair<int32_t, decltype(std::declval<ValueOf>()(std::declval<Values &>()))>> drain(Values &values,
                                                                                                   ValueOf value_of) {
    std::vector<std::pair<int32_t, decltype(std::declval<ValueOf>()(std::declval<Values &>()))>> entries;
    while (true) {
        auto doc = values.next_doc();
        EXPECT_TRUE(doc.has_value()) << (doc ? "" : doc.error().to_string());
        if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
            break;
        }
        auto value = value_of(values);
        EXPECT_TRUE(value.has_value());
        if (value.has_value()) {
            entries.emplace_back(doc.value(), value.value());
        }
    }
    return entries;
}

} // namespace

TEST(SortingDocValuesTest, RenumbersNumericValuesIntoTheSortedOrder) {
    // Old docs 0..3 hold 10, 20, 30, 40; the sort reverses them.
    auto values = std::make_unique<FakeNumericDocValues>(std::vector<int64_t>{10, 20, 30, 40});
    IndexSortDocMapImpl doc_map(std::vector<int32_t>{3, 2, 1, 0});

    auto sorted = SortingNumericDocValues::create(std::move(values), doc_map);
    ASSERT_TRUE(sorted.has_value()) << (sorted ? "" : sorted.error().to_string());

    const auto entries = drain(*sorted.value(), [](NumericDocValues &values) { return values.long_value(); });
    ASSERT_EQ(entries.size(), 4U);
    EXPECT_EQ(entries[0].first, 0);
    EXPECT_EQ(entries[0].second, 40);
    EXPECT_EQ(entries[3].first, 3);
    EXPECT_EQ(entries[3].second, 10);
    EXPECT_EQ(sorted.value()->cost().value(), 4);
}

TEST(SortingDocValuesTest, AdvancesAndSeeksInTheNewOrder) {
    auto values = std::make_unique<FakeNumericDocValues>(std::vector<int64_t>{10, 20, 30, 40});
    IndexSortDocMapImpl doc_map(std::vector<int32_t>{3, 2, 1, 0});
    auto sorted = SortingNumericDocValues::create(std::move(values), doc_map);
    ASSERT_TRUE(sorted.has_value());

    // Seeking works on new doc ids.
    EXPECT_TRUE(sorted.value()->advance_exact(1).value());
    EXPECT_EQ(sorted.value()->long_value().value(), 30);
    EXPECT_TRUE(sorted.value()->advance_exact(3).value());
    EXPECT_EQ(sorted.value()->long_value().value(), 10);

    // Advancing skips to the next new doc id at or after the target.
    auto fresh = SortingNumericDocValues::create(std::make_unique<FakeNumericDocValues>(std::vector<int64_t>{10, 20, 30, 40}),
                                                doc_map);
    ASSERT_TRUE(fresh.has_value());
    EXPECT_EQ(fresh.value()->advance(2).value(), 2);
    EXPECT_EQ(fresh.value()->long_value().value(), 20);
    EXPECT_EQ(fresh.value()->next_doc().value(), 3);
    EXPECT_EQ(fresh.value()->next_doc().value(), NO_MORE_DOCS);
}

TEST(SortingDocValuesTest, SkipsDocumentsWithoutAValue) {
    // Old doc 1 has no value, so only three documents reach the sorted view.
    auto values = std::make_unique<FakeNumericDocValues>(std::vector<int64_t>{10, -1, 30, 40});
    IndexSortDocMapImpl doc_map(std::vector<int32_t>{3, 2, 1, 0});
    auto sorted = SortingNumericDocValues::create(std::move(values), doc_map);
    ASSERT_TRUE(sorted.has_value());

    const auto entries = drain(*sorted.value(), [](NumericDocValues &values) { return values.long_value(); });
    ASSERT_EQ(entries.size(), 3U);
    EXPECT_EQ(entries[0].first, 0);
    EXPECT_EQ(entries[0].second, 40);
    // New doc 2 would be old doc 1, which has no value, so it is skipped.
    EXPECT_EQ(entries[1].first, 1);
    EXPECT_EQ(entries[1].second, 30);
    EXPECT_EQ(entries[2].first, 3);
    EXPECT_EQ(entries[2].second, 10);

    // The document that has no value cannot be seeked to.
    auto probe = SortingNumericDocValues::create(std::make_unique<FakeNumericDocValues>(std::vector<int64_t>{10, -1, 30, 40}),
                                                 doc_map);
    ASSERT_TRUE(probe.has_value());
    EXPECT_TRUE(probe.value()->advance_exact(1).value());
    EXPECT_FALSE(probe.value()->advance_exact(2).value());
}

TEST(SortingDocValuesTest, DropsDeletedDocuments) {
    // The map reports -1 for the document that was deleted before the merge.
    auto values = std::make_unique<FakeNumericDocValues>(std::vector<int64_t>{10, 20, 30, 40});
    // New order: old 3 -> new 0, old 2 -> new 1, old 1 deleted, old 0 -> new 2.
    IndexSortDocMapImpl doc_map(std::vector<int32_t>{3, 2, 0}, 4);
    auto sorted = SortingNumericDocValues::create(std::move(values), doc_map);
    ASSERT_TRUE(sorted.has_value());

    const auto entries = drain(*sorted.value(), [](NumericDocValues &values) { return values.long_value(); });
    ASSERT_EQ(entries.size(), 3U);
    EXPECT_EQ(entries[0].second, 40);
    EXPECT_EQ(entries[1].second, 30);
    EXPECT_EQ(entries[2].second, 10);
}

TEST(SortingDocValuesTest, RenumbersTermOrdinalsAndKeepsTheDictionary) {
    // Terms are a, b, c; old docs hold b, a, c.
    auto values = std::make_unique<FakeSortedDocValues>(std::vector<int32_t>{1, 0, 2},
                                                        std::vector<std::string>{"a", "b", "c"});
    IndexSortDocMapImpl doc_map(std::vector<int32_t>{2, 1, 0});
    auto sorted = SortingSortedDocValues::create(std::move(values), doc_map);
    ASSERT_TRUE(sorted.has_value()) << (sorted ? "" : sorted.error().to_string());

    const auto entries = drain(*sorted.value(), [](SortedDocValues &values) { return values.ord_value(); });
    ASSERT_EQ(entries.size(), 3U);
    // New doc 0 is old doc 2 (term c -> ord 2), then b (1), then a (0).
    EXPECT_EQ(entries[0].first, 0);
    EXPECT_EQ(entries[0].second, 2);
    EXPECT_EQ(entries[1].second, 0);
    EXPECT_EQ(entries[2].second, 1);

    // The dictionary is shared with the underlying segment.
    EXPECT_EQ(sorted.value()->get_value_count().value(), 3);
    const BytesRef term = sorted.value()->lookup_ord(1).value();
    EXPECT_TRUE(std::ranges::equal(term.as_slice(), BytesRef::from_string("b").as_slice()));
}

TEST(SortingDocValuesTest, RejectsMissingValues) {
    IndexSortDocMapImpl doc_map(std::vector<int32_t>{0});
    auto numeric = SortingNumericDocValues::create(nullptr, doc_map);
    ASSERT_FALSE(numeric.has_value());
    EXPECT_EQ(numeric.error().kind(), ErrorKind::kIllegalArgument);

    EXPECT_FALSE(SortingSortedDocValues::create(nullptr, doc_map).has_value());
    EXPECT_FALSE(SortingBinaryDocValues::create(nullptr, doc_map).has_value());
}
