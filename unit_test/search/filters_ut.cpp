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
#include <expected>
#include <memory>
#include <vector>

module lucene_core:ut.filters;

import lucene_core;

using namespace lucene;

namespace {

/// A [`DocIdSetIterator`] over the given documents.
class ListDisi final : public DocIdSetIterator {
public:
    explicit ListDisi(std::vector<int32_t> docs) : docs_(std::move(docs)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        index_ += 1;
        doc_ = index_ < docs_.size() ? docs_[index_] : NO_MORE_DOCS;
        return doc_;
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        while (true) {
            if (index_ + 1 >= docs_.size()) {
                index_ = docs_.size();
                doc_ = NO_MORE_DOCS;
                return doc_;
            }
            index_ += 1;
            if (docs_[index_] >= target) {
                doc_ = docs_[index_];
                return doc_;
            }
        }
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(docs_.size()); }

private:
    std::vector<int32_t> docs_;
    size_t index_{static_cast<size_t>(-1)};
    int32_t doc_{-1};
};

/// A [`NumericDocValues`] over `doc -> value`.
class ListNumericDocValues final : public NumericDocValues {
public:
    ListNumericDocValues(std::vector<int64_t> values, int32_t max_doc) : values_(std::move(values)), max_doc_(max_doc) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override { return advance(doc_ + 1); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        doc_ = target < max_doc_ ? target : NO_MORE_DOCS;
        return doc_;
    }
    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(max_doc_); }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        doc_ = target;
        return target >= 0 && target < max_doc_;
    }
    [[nodiscard]] Result<int64_t> long_value() override { return values_[static_cast<size_t>(doc_)]; }

private:
    std::vector<int64_t> values_;
    int32_t max_doc_;
    int32_t doc_{-1};
};

class FixedScorable final : public Scorable {
public:
    explicit FixedScorable(float score) : score_(score) {}

    [[nodiscard]] Result<float> score() override { return score_; }
    [[nodiscard]] VoidResult set_min_competitive_score(float min_score) override {
        min_competitive_score_ = min_score;
        return {};
    }

    [[nodiscard]] float min_competitive_score() const noexcept { return min_competitive_score_; }

private:
    float score_;
    float min_competitive_score_{-1.0F};
};

} // namespace

TEST(FilterDocIdSetIteratorTest, DelegatesEveryOperation) {
    FilterDocIdSetIterator iterator(std::make_unique<ListDisi>(std::vector<int32_t>{1, 4, 9}));
    EXPECT_EQ(iterator.cost().value(), 3);
    EXPECT_EQ(iterator.next_doc().value(), 1);
    EXPECT_EQ(iterator.doc_id(), 1);
    EXPECT_EQ(iterator.next_doc().value(), 4);
    EXPECT_EQ(iterator.advance(9).value(), 9);
    EXPECT_EQ(iterator.next_doc().value(), NO_MORE_DOCS);
    EXPECT_EQ(iterator.in().doc_id(), NO_MORE_DOCS);
}

TEST(FilterScorableTest, DelegatesScoreAndMinimum) {
    FixedScorable inner(2.5F);
    FilterScorable scorable(inner);
    EXPECT_FLOAT_EQ(scorable.score().value(), 2.5F);
    ASSERT_TRUE(scorable.set_min_competitive_score(1.5F).has_value());
    EXPECT_FLOAT_EQ(inner.min_competitive_score(), 1.5F);
    EXPECT_EQ(&scorable.in(), &inner);
}

TEST(FilterNumericDocValuesTest, DelegatesTheCursor) {
    FilterNumericDocValues values(std::make_unique<ListNumericDocValues>(std::vector<int64_t>{10, 11, 12}, 3));
    ASSERT_TRUE(values.advance_exact(1).value());
    EXPECT_EQ(values.long_value().value(), 11);
    EXPECT_EQ(values.doc_id(), 1);
    EXPECT_EQ(values.next_doc().value(), 2);
    EXPECT_EQ(values.long_value().value(), 12);
    EXPECT_EQ(values.next_doc().value(), NO_MORE_DOCS);
    EXPECT_EQ(values.cost().value(), 3);
    EXPECT_FALSE(values.advance_exact(3).value());
}
