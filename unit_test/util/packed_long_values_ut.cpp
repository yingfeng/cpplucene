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
#include <vector>

module lucene_core:ut.packed_long_values;

import lucene_core;

using namespace lucene;

namespace {

std::vector<int64_t> iterate(const PackedLongValues &values) {
    std::vector<int64_t> result;
    auto iterator = values.iterator();
    while (iterator->has_next().value()) {
        result.push_back(iterator->next().value());
    }
    return result;
}

} // namespace

TEST(PackedLongValuesTest, EmptyBuilderBuildsEmptyValues) {
    PackedLongValues::Builder builder(8, 64, 0, false);
    auto values = builder.build();
    ASSERT_TRUE(values.has_value());
    EXPECT_EQ(values.value().size(), 0);
    EXPECT_TRUE(values.value().is_empty());
    EXPECT_EQ(values.value().page_count(), 0);
    EXPECT_TRUE(iterate(values.value()).empty());
    EXPECT_FALSE(values.value().get(0).has_value());
}

TEST(PackedLongValuesTest, SinglePageRoundTrips) {
    PackedLongValues::Builder builder(8, 64, 0, false);
    for (int64_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(builder.add(i * 7).has_value());
    }
    EXPECT_EQ(builder.size(), 5);

    auto values = builder.build();
    ASSERT_TRUE(values.has_value());
    EXPECT_EQ(values.value().size(), 5);
    EXPECT_EQ(values.value().page_count(), 1);
    for (int64_t i = 0; i < 5; ++i) {
        EXPECT_EQ(values.value().get(static_cast<size_t>(i)).value(), i * 7);
    }
    EXPECT_EQ(iterate(values.value()), (std::vector<int64_t>{0, 7, 14, 21, 28}));
    EXPECT_FALSE(values.value().get(5).has_value());
    EXPECT_GT(values.value().ram_bytes_used(), 0);
}

TEST(PackedLongValuesTest, MultiplePagesDecodeIndependently) {
    PackedLongValues::Builder builder(4, 64, 0, false);
    std::vector<int64_t> expected;
    for (int64_t i = 0; i < 11; ++i) {
        const int64_t value = (i * i * 3) + 1;
        expected.push_back(value);
        ASSERT_TRUE(builder.add(value).has_value());
    }

    auto values = builder.build();
    ASSERT_TRUE(values.has_value());
    EXPECT_EQ(values.value().size(), 11);
    EXPECT_EQ(values.value().page_count(), 3);
    EXPECT_EQ(iterate(values.value()), expected);

    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(values.value().get(i).value(), expected[i]);
    }

    // Blocks are decoded on demand and report their real length.
    std::vector<int64_t> block;
    EXPECT_EQ(values.value().decode_block(0, block).value(), 4);
    EXPECT_EQ(block.size(), 4);
    EXPECT_EQ(values.value().decode_block(2, block).value(), 3);
    EXPECT_EQ(block, (std::vector<int64_t>{expected[8], expected[9], expected[10]}));
    EXPECT_FALSE(values.value().decode_block(3, block).has_value());
}

TEST(PackedLongValuesTest, DeltaBuilderUsesPerPageMinimum) {
    PackedLongValues::Builder builder(4, 64, 0, true);
    // Monotonically increasing values: each page is stored relative to its min.
    std::vector<int64_t> expected;
    for (int64_t i = 0; i < 10; ++i) {
        const int64_t value = 1000 + i * 13;
        expected.push_back(value);
        ASSERT_TRUE(builder.add(value).has_value());
    }

    auto values = builder.build();
    ASSERT_TRUE(values.has_value());
    EXPECT_EQ(values.value().page_count(), 3);
    EXPECT_EQ(iterate(values.value()), expected);
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(values.value().get(i).value(), expected[i]);
    }
}

TEST(PackedLongValuesTest, DeltaBuilderFallsBackWhenValuesDecrease) {
    PackedLongValues::Builder builder(4, 64, 0, true);
    std::vector<int64_t> expected{5, 9, 2, 40, 41, 3};
    for (const auto value : expected) {
        ASSERT_TRUE(builder.add(value).has_value());
    }

    auto values = builder.build();
    ASSERT_TRUE(values.has_value());
    // The first page is increasing, the second one is not: both must decode.
    EXPECT_EQ(iterate(values.value()), expected);
}

TEST(PackedLongValuesTest, BuilderCannotBeBuiltTwice) {
    PackedLongValues::Builder builder(4, 64, 0, false);
    ASSERT_TRUE(builder.add(1).has_value());
    ASSERT_TRUE(builder.build().has_value());
    EXPECT_FALSE(builder.build().has_value());
    EXPECT_FALSE(builder.add(2).has_value());
}
