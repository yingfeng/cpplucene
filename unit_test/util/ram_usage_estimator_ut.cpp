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
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

module lucene_core:ut.ram_usage_estimator;

import lucene_core;

using namespace lucene;

TEST(RamUsageEstimatorTest, PrimitiveSizes) {
    EXPECT_EQ(primitive_size(PrimitiveType::Boolean), 1);
    EXPECT_EQ(primitive_size(PrimitiveType::Byte), 1);
    EXPECT_EQ(primitive_size(PrimitiveType::Char), 4);
    EXPECT_EQ(primitive_size(PrimitiveType::Short), 2);
    EXPECT_EQ(primitive_size(PrimitiveType::Int), 4);
    EXPECT_EQ(primitive_size(PrimitiveType::Float), 4);
    EXPECT_EQ(primitive_size(PrimitiveType::Double), 8);
    EXPECT_EQ(primitive_size(PrimitiveType::Long), 8);
    EXPECT_EQ(QUERY_DEFAULT_RAM_BYTES_USED, 1024);
    EXPECT_EQ(ONE_KB, 1024);
    EXPECT_EQ(ONE_MB, 1024 * 1024);
    EXPECT_EQ(ONE_GB, 1024 * 1024 * 1024);
}

TEST(RamUsageEstimatorTest, ArrayAndContainerSizes) {
    // An empty container retains nothing.
    EXPECT_EQ(size_of_array_capacity<int32_t>(0), 0);
    const std::vector<int32_t> ints(10, 0);
    EXPECT_GE(size_of_vec(ints), static_cast<int64_t>(10 * sizeof(int32_t)));

    const std::deque<int64_t> longs(4, 0);
    EXPECT_GE(size_of_vec_deque(longs), static_cast<int64_t>(4 * sizeof(int64_t)));

    const std::vector<std::string> words{"alpha", "beta"};
    EXPECT_GE(size_of_string_vec(words), size_of_vec(words) + 9);

    // A map costs at least its bucket array.
    const std::unordered_map<int32_t, int32_t> map{{1, 2}, {3, 4}};
    EXPECT_GE(size_of_hash_map(map), 0);

    // Slices count elements, not the inline descriptors.
    const std::vector<int32_t> backing{1, 2, 3};
    EXPECT_EQ(size_of_slice<int32_t>(backing), static_cast<int64_t>(3 * sizeof(int32_t)));
    EXPECT_EQ(size_of_entry<int64_t>(), 8);
}

TEST(RamUsageEstimatorTest, StringSizesIgnoreTheObject) {
    EXPECT_EQ(size_of_string(""), 0);
    EXPECT_EQ(size_of_string("abc"), 3);
    const std::string wide(1000, 'x');
    EXPECT_EQ(size_of_string(wide), 1000);
}

TEST(RamUsageEstimatorTest, HumanReadableUnits) {
    EXPECT_EQ(human_readable_units(0), "0 bytes");
    EXPECT_EQ(human_readable_units(512), "512 bytes");
    EXPECT_EQ(human_readable_units(ONE_KB), "1 KB");
    EXPECT_EQ(human_readable_units(1536), "1.5 KB");
    EXPECT_EQ(human_readable_units(ONE_MB), "1 MB");
    EXPECT_EQ(human_readable_units(ONE_MB + ONE_MB / 4), "1.2 MB");
    EXPECT_EQ(human_readable_units(ONE_GB), "1 GB");
    // A value just below the next unit stays in the smaller one.
    EXPECT_EQ(human_readable_units(ONE_KB - 1), "1023 bytes");
}
