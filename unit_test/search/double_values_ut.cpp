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
#include <unordered_map>

module lucene_core:ut.double_values;

import lucene_core;

using namespace lucene;

namespace {

/// Values only for the documents present in `values`.
class MapDoubleValues final : public DoubleValues {
public:
    explicit MapDoubleValues(std::unordered_map<int32_t, double> values) : values_(std::move(values)) {}

    [[nodiscard]] Result<double> double_value() const override {
        auto it = values_.find(doc_);
        if (it == values_.end()) {
            return err(LuceneError::illegal_state("no value for the current document"));
        }
        return it->second;
    }

    [[nodiscard]] Result<bool> advance_exact(int32_t doc) override {
        doc_ = doc;
        return values_.contains(doc);
    }

private:
    std::unordered_map<int32_t, double> values_;
    int32_t doc_{-1};
};

} // namespace

TEST(WithDefaultDoubleValuesTest, FallsBackToTheDefault) {
    auto inner = std::make_unique<MapDoubleValues>(std::unordered_map<int32_t, double>{{2, 2.5}, {5, 7.0}});
    WithDefaultDoubleValues values(std::move(inner), -1.0);

    // A document with a value reports it.
    ASSERT_TRUE(values.advance_exact(2).value());
    EXPECT_DOUBLE_EQ(values.double_value().value(), 2.5);

    // A document without one reports the default, but still counts as present.
    ASSERT_TRUE(values.advance_exact(3).value());
    EXPECT_DOUBLE_EQ(values.double_value().value(), -1.0);

    ASSERT_TRUE(values.advance_exact(5).value());
    EXPECT_DOUBLE_EQ(values.double_value().value(), 7.0);
}

TEST(EmptyDoubleValuesTest, NeverHasValues) {
    EmptyDoubleValues values;
    EXPECT_FALSE(values.advance_exact(0).value());
    EXPECT_FALSE(values.advance_exact(42).value());
    EXPECT_FALSE(values.double_value().has_value());
}

TEST(MinDocIteratorTest, SkipsToTheSegmentMinimum) {
    MinDocIterator iterator(5, 10);
    EXPECT_EQ(iterator.doc_id(), -1);
    EXPECT_EQ(iterator.cost().value(), 5);

    // The first next_doc() lands on the segment's smallest document.
    EXPECT_EQ(iterator.next_doc().value(), 5);
    EXPECT_EQ(iterator.next_doc().value(), 6);

    // A target below the minimum is raised to it ...
    MinDocIterator fresh(5, 10);
    EXPECT_EQ(fresh.advance(2).value(), 5);
    // ... while `advance` past the end exhausts the iterator.
    MinDocIterator late(5, 10);
    EXPECT_EQ(late.advance(20).value(), NO_MORE_DOCS);
    EXPECT_EQ(late.next_doc().value(), NO_MORE_DOCS);
}

TEST(MinDocIteratorTest, EmptiesWhenTheRangeIsEmpty) {
    MinDocIterator iterator(5, 5);
    EXPECT_EQ(iterator.next_doc().value(), NO_MORE_DOCS);
    EXPECT_EQ(iterator.cost().value(), 0);
}

TEST(DoubleValuesSourceTest, AbstractDefaults) {
    // A source that supports nothing beyond the required methods.
    class ConstantSource final : public DoubleValuesSource {
    public:
        [[nodiscard]] Result<std::unique_ptr<DoubleValues>>
        get_values(const LeafReaderContext & /*ctx*/, std::unique_ptr<DoubleValues> /*scores*/) const override {
            return std::unique_ptr<DoubleValues>(new EmptyDoubleValues());
        }
        [[nodiscard]] bool needs_scores() const noexcept override { return false; }
    };

    ConstantSource source;
    const auto context = LeafReaderContext(nullptr, 0, 0, TopParentMeta{1, 1, Identity{}});
    EXPECT_FALSE(source.needs_scores());
    EXPECT_TRUE(source.is_cacheable(context));
    // The defaults report that the optional operations are not available.
    const Explanation score_explanation = Explanation::match_no_details(1.0F, "score");
    EXPECT_FALSE(source.explain(context, 0, score_explanation).has_value());
    auto values = source.get_values(context, nullptr);
    ASSERT_TRUE(values.has_value());
    EXPECT_FALSE(values.value()->advance_exact(0).value());
    EXPECT_EQ(source.get_sort_field(false).value(), nullptr);
}
