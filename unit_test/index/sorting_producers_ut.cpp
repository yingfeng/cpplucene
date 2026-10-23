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

module lucene_core:ut.sorting_producers;

import lucene_core;

using namespace lucene;

namespace {

/// Numeric doc values over `values`.
class FakeNumericDocValues final : public NumericDocValues {
public:
    explicit FakeNumericDocValues(std::vector<int64_t> values) : values_(std::move(values)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        doc_ += 1;
        if (static_cast<size_t>(doc_) >= values_.size()) {
            doc_ = NO_MORE_DOCS;
        }
        return doc_;
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        doc_ = target;
        if (static_cast<size_t>(doc_) >= values_.size()) {
            doc_ = NO_MORE_DOCS;
        }
        return doc_;
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(values_.size()); }

    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        if (target < 0 || static_cast<size_t>(target) >= values_.size()) {
            return false;
        }
        doc_ = target;
        return true;
    }

    [[nodiscard]] Result<int64_t> long_value() override { return values_[static_cast<size_t>(doc_)]; }

private:
    std::vector<int64_t> values_;
    int32_t doc_{-1};
};

/// A norms producer over one field.
class FakeNormsProducer final : public NormsProducer {
public:
    explicit FakeNormsProducer(std::vector<int64_t> values) : values_(std::move(values)) {}

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norms(const FieldInfo &/*field*/) const override {
        return std::unique_ptr<NumericDocValues>(new FakeNumericDocValues(values_));
    }

    [[nodiscard]] VoidResult check_integrity() const override { return {}; }

    [[nodiscard]] VoidResult close() const override { return {}; }

private:
    std::vector<int64_t> values_;
};

/// A doc values producer with numeric and (optionally) sorted values only.
class FakeDocValuesProducer final : public DocValuesProducer {
public:
    FakeDocValuesProducer(std::vector<int64_t> numeric, bool has_sorted)
            : numeric_(std::move(numeric)), has_sorted_(has_sorted) {}

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric(const FieldInfo &/*field*/) const override {
        return std::unique_ptr<NumericDocValues>(new FakeNumericDocValues(numeric_));
    }

    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted(const FieldInfo &/*field*/) const override {
        if (!has_sorted_) {
            return std::unique_ptr<SortedDocValues>();
        }
        return err(LuceneError::unsupported_operation("the fake has no sorted values"));
    }

    [[nodiscard]] VoidResult check_integrity() const override { return {}; }

    [[nodiscard]] VoidResult close() const override { return {}; }

private:
    std::vector<int64_t> numeric_;
    bool has_sorted_{false};
};

/// The map that reverses a three-document segment.
[[nodiscard]] std::shared_ptr<const IndexSortDocMap> reversed_map() {
    return std::make_shared<const IndexSortDocMapImpl>(std::vector<int32_t>{2, 1, 0});
}

} // namespace

TEST(SortingNormsProducerTest, RenumbersNormsIntoTheSortedOrder) {
    auto fake = std::make_shared<FakeNormsProducer>(std::vector<int64_t>{10, 20, 30});
    auto producer = SortingNormsProducer::create(fake, reversed_map());
    ASSERT_TRUE(producer.has_value()) << (producer ? "" : producer.error().to_string());

    FieldInfo field;
    field.name = "f";
    field.index_options = IndexOptions::DocsAndFreqs;
    auto norms = producer.value()->get_norms(field);
    ASSERT_TRUE(norms.has_value()) << (norms ? "" : norms.error().to_string());
    ASSERT_TRUE(norms.value() != nullptr);

    // Sorted document 0 is old document 2.
    EXPECT_EQ(norms.value()->next_doc().value(), 0);
    EXPECT_EQ(norms.value()->long_value().value(), 30);
    EXPECT_EQ(norms.value()->next_doc().value(), 1);
    EXPECT_EQ(norms.value()->long_value().value(), 20);
    EXPECT_EQ(norms.value()->next_doc().value(), 2);
    EXPECT_EQ(norms.value()->long_value().value(), 10);
    EXPECT_EQ(norms.value()->next_doc().value(), NO_MORE_DOCS);
}

TEST(SortingNormsProducerTest, WrapsTheMergeInstance) {
    auto fake = std::make_shared<FakeNormsProducer>(std::vector<int64_t>{10, 20, 30});
    auto producer = SortingNormsProducer::create(fake, reversed_map());
    ASSERT_TRUE(producer.has_value());

    // The fake reports no merge instance, so the wrapper reuses the producer.
    auto instance = producer.value()->get_merge_instance();
    ASSERT_TRUE(instance.has_value()) << (instance ? "" : instance.error().to_string());
    ASSERT_TRUE(instance.value() != nullptr);

    FieldInfo field;
    field.name = "f";
    auto norms = instance.value()->get_norms(field);
    ASSERT_TRUE(norms.has_value());
    ASSERT_TRUE(norms.value() != nullptr);
    EXPECT_EQ(norms.value()->advance(0).value(), 0);
    EXPECT_EQ(norms.value()->long_value().value(), 30);
}

TEST(SortingDocValuesProducerTest, RenumbersNumericValues) {
    auto fake = std::make_shared<FakeDocValuesProducer>(std::vector<int64_t>{7, 8, 9}, false);
    auto producer = SortingDocValuesProducer::create(fake, reversed_map());
    ASSERT_TRUE(producer.has_value()) << (producer ? "" : producer.error().to_string());

    FieldInfo field;
    field.name = "f";
    auto values = producer.value()->get_numeric(field);
    ASSERT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
    ASSERT_TRUE(values.value() != nullptr);
    EXPECT_EQ(values.value()->advance_exact(0).value(), true);
    EXPECT_EQ(values.value()->long_value().value(), 9);
    EXPECT_EQ(values.value()->advance_exact(2).value(), true);
    EXPECT_EQ(values.value()->long_value().value(), 7);
}

TEST(SortingDocValuesProducerTest, ForwardsAnAbsentField) {
    auto fake = std::make_shared<FakeDocValuesProducer>(std::vector<int64_t>{1}, false);
    auto producer = SortingDocValuesProducer::create(fake, reversed_map());
    ASSERT_TRUE(producer.has_value());

    FieldInfo field;
    field.name = "missing";
    auto sorted = producer.value()->get_sorted(field);
    ASSERT_TRUE(sorted.has_value()) << (sorted ? "" : sorted.error().to_string());
    EXPECT_TRUE(sorted.value() == nullptr);

    // A field the wrapped producer rejects stays rejected.
    auto rejected = producer.value()->get_sorted_numeric(field);
    ASSERT_FALSE(rejected.has_value());
}

TEST(SortingProducersTest, RejectsNullArguments) {
    EXPECT_FALSE(SortingNormsProducer::create(nullptr, reversed_map()).has_value());
    EXPECT_FALSE(SortingNormsProducer::create(std::make_shared<FakeNormsProducer>(std::vector<int64_t>{}), nullptr)
                         .has_value());
    EXPECT_FALSE(SortingDocValuesProducer::create(nullptr, reversed_map()).has_value());
    EXPECT_FALSE(SortingDocValuesProducer::create(std::make_shared<FakeDocValuesProducer>(std::vector<int64_t>{}, false),
                                                 nullptr)
                         .has_value());
}
