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

module lucene_core:ut.bkd_radix_selector;

import lucene_core;

using namespace lucene;

namespace {

BKDConfig two_dim_config() {
    auto config = BKDConfig::create(2, 2, 4, 512);
    EXPECT_TRUE(config.has_value());
    return config.value();
}

std::vector<uint8_t> pack(int32_t dim0, int32_t dim1) {
    std::vector<uint8_t> packed(8, 0);
    BitUtil::set_i32_be(packed, 0, dim0);
    BitUtil::set_i32_be(packed, 4, dim1);
    return packed;
}

int32_t dim_of(HeapPointWriter &writer, size_t index, size_t dim) {
    return BitUtil::get_i32_be(writer.packed_value(index).packed_value(), dim);
}

std::vector<int32_t> dims_of(HeapPointWriter &writer, size_t dim) {
    std::vector<int32_t> values;
    for (size_t i = 0; i < writer.count(); ++i) {
        values.push_back(dim_of(writer, i, dim));
    }
    return values;
}

} // namespace

TEST(BkdRadixSelectorTest, SelectPartitionsAroundThePoint) {
    const BKDConfig config = two_dim_config();
    HeapPointWriter writer(config, 8);
    const std::vector<int32_t> values{5, 3, 8, 1, 9, 2, 7, 4};
    for (size_t i = 0; i < values.size(); ++i) {
        ASSERT_TRUE(writer.append_bytes(pack(values[i], 0), static_cast<int32_t>(i)).has_value());
    }
    ASSERT_TRUE(writer.close().has_value());

    BkdRadixSelector selector(writer);
    auto partition = selector.select(0, 8, 4, 0);
    ASSERT_TRUE(partition.has_value()) << (partition ? "" : partition.error().to_string());
    ASSERT_EQ(partition->size(), 4u);
    const int32_t pivot = BitUtil::get_i32_be(*partition, 0);

    const auto after = dims_of(writer, 0);
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_LE(after[i], pivot) << "left index " << i;
    }
    for (size_t i = 4; i < 8; ++i) {
        EXPECT_GE(after[i], pivot) << "right index " << i;
    }
    // The partition value must be one of the written values.
    EXPECT_NE(std::find(values.begin(), values.end(), pivot), values.end());
    // All eight points are still present.
    std::vector<int32_t> sorted = after;
    std::sort(sorted.begin(), sorted.end());
    EXPECT_EQ(sorted, (std::vector<int32_t>{1, 2, 3, 4, 5, 7, 8, 9}));
}

TEST(BkdRadixSelectorTest, SelectWorksOnSortedAndConstantInput) {
    const BKDConfig config = two_dim_config();

    {
        HeapPointWriter writer(config, 6);
        for (int32_t i = 0; i < 6; ++i) {
            ASSERT_TRUE(writer.append_bytes(pack(i, 0), i).has_value());
        }
        ASSERT_TRUE(writer.close().has_value());
        BkdRadixSelector selector(writer);
        auto partition = selector.select(0, 6, 3, 0);
        ASSERT_TRUE(partition.has_value());
        EXPECT_EQ(BitUtil::get_i32_be(*partition, 0), 3);
    }

    {
        // Every point equal: any partition point must still satisfy the
        // post-condition, and select must terminate.
        HeapPointWriter writer(config, 6);
        for (int32_t i = 0; i < 6; ++i) {
            ASSERT_TRUE(writer.append_bytes(pack(7, 0), i).has_value());
        }
        ASSERT_TRUE(writer.close().has_value());
        BkdRadixSelector selector(writer);
        auto partition = selector.select(0, 6, 2, 0);
        ASSERT_TRUE(partition.has_value());
        EXPECT_EQ(BitUtil::get_i32_be(*partition, 0), 7);
    }
}

TEST(BkdRadixSelectorTest, SelectUsesTheRequestedDimensionAndPrefix) {
    const BKDConfig config = two_dim_config();
    HeapPointWriter writer(config, 6);
    // Dimension 0 is constant, so partitioning by dimension 1 is what matters.
    const std::vector<int32_t> second{30, 10, 50, 20, 60, 40};
    for (size_t i = 0; i < second.size(); ++i) {
        ASSERT_TRUE(writer.append_bytes(pack(1, second[i]), static_cast<int32_t>(i)).has_value());
    }
    ASSERT_TRUE(writer.close().has_value());

    BkdRadixSelector selector(writer);
    auto partition = selector.select(0, 6, 3, 4);
    ASSERT_TRUE(partition.has_value()) << (partition ? "" : partition.error().to_string());
    EXPECT_EQ(BitUtil::get_i32_be(*partition, 0), 40);

    const auto after = dims_of(writer, 4);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_LE(after[i], 40);
    }
    for (size_t i = 3; i < 6; ++i) {
        EXPECT_GE(after[i], 40);
    }

    // Skipping the first two bytes of dimension 0 (all zero here) changes
    // nothing but must still be accepted.
    auto with_prefix = selector.select(0, 6, 2, 0, 2);
    ASSERT_TRUE(with_prefix.has_value()) << (with_prefix ? "" : with_prefix.error().to_string());
}

TEST(BkdRadixSelectorTest, SortOrdersTheSlice) {
    const BKDConfig config = two_dim_config();
    HeapPointWriter writer(config, 10);
    const std::vector<int32_t> values{9, 4, 7, 1, 8, 0, 6, 3, 5, 2};
    for (size_t i = 0; i < values.size(); ++i) {
        ASSERT_TRUE(writer.append_bytes(pack(values[i], static_cast<int32_t>(100 - values[i])),
                                        static_cast<int32_t>(i))
                        .has_value());
    }
    ASSERT_TRUE(writer.close().has_value());

    BkdRadixSelector selector(writer);
    ASSERT_TRUE(selector.sort(0, 10, 0).has_value());

    const auto sorted = dims_of(writer, 0);
    for (size_t i = 1; i < sorted.size(); ++i) {
        EXPECT_LE(sorted[i - 1], sorted[i]) << "index " << i;
    }
    EXPECT_EQ(sorted.front(), 0);
    EXPECT_EQ(sorted.back(), 9);

    // A sub-range sort leaves the rest of the block alone.
    HeapPointWriter other(config, 5);
    for (int32_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(other.append_bytes(pack(4 - i, 0), i).has_value());
    }
    ASSERT_TRUE(other.close().has_value());
    BkdRadixSelector other_selector(other);
    ASSERT_TRUE(other_selector.sort(1, 4, 0).has_value());
    const auto partial = dims_of(other, 0);
    EXPECT_EQ(partial[0], 4);
    EXPECT_EQ(partial[1], 1);
    EXPECT_EQ(partial[2], 2);
    EXPECT_EQ(partial[3], 3);
    EXPECT_EQ(partial[4], 0);
}

TEST(BkdRadixSelectorTest, RejectsInvalidArguments) {
    const BKDConfig config = two_dim_config();
    HeapPointWriter writer(config, 3);
    for (int32_t i = 0; i < 3; ++i) {
        ASSERT_TRUE(writer.append_bytes(pack(i, 0), i).has_value());
    }
    ASSERT_TRUE(writer.close().has_value());

    BkdRadixSelector selector(writer);
    EXPECT_FALSE(selector.select(0, 3, 3, 0).has_value());  // partition point == to
    EXPECT_FALSE(selector.select(2, 3, 0, 0).has_value());  // partition point < from
    EXPECT_FALSE(selector.select(0, 3, 1, 8).has_value());  // dimension outside the point
    EXPECT_FALSE(selector.select(0, 3, 1, 0, 4).has_value());  // prefix consumes the dimension
    EXPECT_FALSE(selector.sort(0, 4, 0).has_value());          // slice past the count
}
