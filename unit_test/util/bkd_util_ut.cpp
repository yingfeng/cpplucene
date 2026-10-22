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

module lucene_core:ut.bkd_util;

import lucene_core;

using namespace lucene;

namespace {

std::vector<uint8_t> bytes_of(std::initializer_list<int> values) {
    std::vector<uint8_t> bytes;
    bytes.reserve(values.size());
    for (const int value : values) {
        bytes.push_back(static_cast<uint8_t>(value));
    }
    return bytes;
}

std::vector<uint8_t> repeated(uint8_t value, size_t count) { return std::vector<uint8_t>(count, value); }

} // namespace

TEST(BKDConfigTest, ValidatesAndComputesSizes) {
    auto config = BKDConfig::create(2, 1, 4, 512);
    ASSERT_TRUE(config.has_value()) << (config ? "" : config.error().to_string());
    EXPECT_EQ(config->packed_bytes_length(), 8u);
    EXPECT_EQ(config->packed_index_bytes_length(), 4u);
    EXPECT_EQ(config->bytes_per_doc(), 8u + sizeof(int32_t));

    EXPECT_EQ(BKDConfig::DEFAULT_MAX_POINTS_IN_LEAF_NODE, 512u);
    EXPECT_EQ(BKDConfig::MAX_DIMS, 16u);
    EXPECT_EQ(BKDConfig::MAX_INDEX_DIMS, 8u);
}

TEST(BKDConfigTest, RejectsInvalidParameters) {
    EXPECT_FALSE(BKDConfig::create(0, 1, 4, 512).has_value());
    EXPECT_FALSE(BKDConfig::create(17, 1, 4, 512).has_value());
    EXPECT_FALSE(BKDConfig::create(2, 0, 4, 512).has_value());
    EXPECT_FALSE(BKDConfig::create(2, 9, 4, 512).has_value());
    EXPECT_FALSE(BKDConfig::create(1, 2, 4, 512).has_value());
    EXPECT_FALSE(BKDConfig::create(2, 1, 0, 512).has_value());
    EXPECT_FALSE(BKDConfig::create(2, 1, 4, 0).has_value());
}

TEST(BKDUtilTest, CommonPrefixLength8) {
    const auto all_zero = repeated(0, 8);
    EXPECT_EQ(BKDUtil::common_prefix_length8(all_zero, 0, all_zero, 0), 8);

    // Differ in the very first byte.
    const auto first_differs = bytes_of({1, 0, 0, 0, 0, 0, 0, 0});
    EXPECT_EQ(BKDUtil::common_prefix_length8(first_differs, 0, all_zero, 0), 0);

    // Differ in the third byte (index 2).
    const auto third_differs = bytes_of({0, 0, 9, 0, 0, 0, 0, 0});
    EXPECT_EQ(BKDUtil::common_prefix_length8(third_differs, 0, all_zero, 0), 2);

    // Differ in the last byte.
    const auto last_differs = bytes_of({0, 0, 0, 0, 0, 0, 0, 7});
    EXPECT_EQ(BKDUtil::common_prefix_length8(last_differs, 0, all_zero, 0), 7);

    // Offsets are honoured.
    const auto padded = bytes_of({9, 9, 0, 0, 0, 0, 0, 0, 0, 0});
    EXPECT_EQ(BKDUtil::common_prefix_length8(padded, 2, all_zero, 0), 8);
}

TEST(BKDUtilTest, CommonPrefixLength4) {
    const auto all_zero = repeated(0, 4);
    EXPECT_EQ(BKDUtil::common_prefix_length4(all_zero, 0, all_zero, 0), 4);
    EXPECT_EQ(BKDUtil::common_prefix_length4(bytes_of({1, 0, 0, 0}), 0, all_zero, 0), 0);
    EXPECT_EQ(BKDUtil::common_prefix_length4(bytes_of({0, 5, 0, 0}), 0, all_zero, 0), 1);
    EXPECT_EQ(BKDUtil::common_prefix_length4(bytes_of({0, 0, 5, 3}), 0, all_zero, 0), 2);
    EXPECT_EQ(BKDUtil::common_prefix_length4(bytes_of({0, 0, 0, 5}), 0, all_zero, 0), 3);
}

TEST(BKDUtilTest, CommonPrefixLengthN) {
    const auto left = bytes_of({1, 2, 3, 4, 5, 6});
    const auto right = bytes_of({1, 2, 9, 9, 9, 9});
    EXPECT_EQ(BKDUtil::common_prefix_length_n(left, 0, right, 0, 6), 2);
    EXPECT_EQ(BKDUtil::common_prefix_length_n(left, 0, left, 0, 6), 6);
    EXPECT_EQ(BKDUtil::common_prefix_length_n(left, 2, left, 2, 4), 4);
    EXPECT_EQ(BKDUtil::common_prefix_length_n(left, 0, right, 0, 1), 1);
}

TEST(BKDUtilTest, EqualsWideAndNarrow) {
    const auto left = bytes_of({1, 2, 3, 4, 5, 6, 7, 8});
    const auto same = bytes_of({1, 2, 3, 4, 5, 6, 7, 8});
    const auto different_tail = bytes_of({1, 2, 3, 4, 5, 6, 7, 9});

    EXPECT_TRUE(BKDUtil::equals8(left, 0, same, 0));
    EXPECT_FALSE(BKDUtil::equals8(left, 0, different_tail, 0));
    EXPECT_TRUE(BKDUtil::equals4(left, 0, same, 0));
    EXPECT_TRUE(BKDUtil::equals4(left, 0, different_tail, 0));
    EXPECT_FALSE(BKDUtil::equals4(bytes_of({9, 2, 3, 4}), 0, left, 0));
}

TEST(BKDUtilTest, PredicateAndComparatorDispatchByWidth) {
    const auto left = bytes_of({1, 2, 3, 4, 5, 6, 7, 8});
    const auto same = bytes_of({1, 2, 3, 4, 5, 6, 7, 8});

    const ByteArrayPredicate predicate8{8};
    EXPECT_TRUE(predicate8.test(left, 0, same, 0));

    const ByteArrayPredicate predicate4{4};
    EXPECT_TRUE(predicate4.test(left, 0, same, 0));

    const ByteArrayPredicate predicate3{3};
    EXPECT_TRUE(predicate3.test(left, 0, same, 0));
    EXPECT_FALSE(predicate3.test(left, 1, same, 0));

    const ByteArrayComparator comparator8{8};
    EXPECT_EQ(comparator8.common_prefix_length(left, 0, same, 0), 8);
    const ByteArrayComparator comparator4{4};
    EXPECT_EQ(comparator4.common_prefix_length(left, 0, same, 0), 4);
    const ByteArrayComparator comparator6{6};
    EXPECT_EQ(comparator6.common_prefix_length(left, 0, bytes_of({1, 2, 3, 9, 9, 9}), 0), 3);
    EXPECT_EQ(comparator6.common_prefix_length(left, 0, left, 0), 6);
}
