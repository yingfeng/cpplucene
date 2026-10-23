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
#include <limits>
#include <string>
#include <vector>

module lucene_core:ut.doc_values_long_hash_set;

import lucene_core;

using namespace lucene;

TEST(DocValuesLongHashSetTest, ContainsTheValuesItWasBuiltWith) {
    auto set = DocValuesLongHashSet::create(std::vector<int64_t>{1, 2, 3});
    ASSERT_TRUE(set.has_value()) << set.error().to_string();

    EXPECT_EQ(set.value().size(), 3U);
    EXPECT_TRUE(set.value().contains(1));
    EXPECT_TRUE(set.value().contains(2));
    EXPECT_TRUE(set.value().contains(3));
    EXPECT_FALSE(set.value().contains(4));
    EXPECT_FALSE(set.value().contains(0));
    EXPECT_EQ(set.value().min_value(), 1);
    EXPECT_EQ(set.value().max_value(), 3);
}

TEST(DocValuesLongHashSetTest, TracksTheMissingValueSeparately) {
    const int64_t missing = std::numeric_limits<int64_t>::min();
    auto set = DocValuesLongHashSet::create(std::vector<int64_t>{missing, 5, 7});
    ASSERT_TRUE(set.has_value());

    EXPECT_TRUE(set.value().has_missing_value());
    EXPECT_TRUE(set.value().contains(missing));
    EXPECT_EQ(set.value().size(), 3U);
    // The table uses the missing value as its empty marker, so the flag is what
    // tells a present `INT64_MIN` apart from an empty slot.
    EXPECT_EQ(set.value().table().size() & (set.value().table().size() - 1), 0U);
    EXPECT_EQ(set.value().min_value(), missing);
    EXPECT_EQ(set.value().max_value(), 7);

    auto without = DocValuesLongHashSet::create(std::vector<int64_t>{5, 7});
    ASSERT_TRUE(without.has_value());
    EXPECT_FALSE(without.value().has_missing_value());
    EXPECT_FALSE(without.value().contains(missing));
}

TEST(DocValuesLongHashSetTest, DeduplicatesRepeatedValues) {
    auto set = DocValuesLongHashSet::create(std::vector<int64_t>{3, 3, 4, 4, 4});
    ASSERT_TRUE(set.has_value());
    EXPECT_EQ(set.value().size(), 2U);
    EXPECT_TRUE(set.value().contains(3));
    EXPECT_TRUE(set.value().contains(4));
}

TEST(DocValuesLongHashSetTest, HandlesNegativeAndLargeValues) {
    auto set = DocValuesLongHashSet::create(
            std::vector<int64_t>{-5, -1, 0, std::numeric_limits<int64_t>::max() - 1});
    ASSERT_TRUE(set.has_value());

    EXPECT_TRUE(set.value().contains(-5));
    EXPECT_TRUE(set.value().contains(-1));
    EXPECT_TRUE(set.value().contains(0));
    EXPECT_TRUE(set.value().contains(std::numeric_limits<int64_t>::max() - 1));
    EXPECT_FALSE(set.value().contains(std::numeric_limits<int64_t>::max()));
    EXPECT_EQ(set.value().min_value(), -5);
}

TEST(DocValuesLongHashSetTest, HandlesAnEmptySet) {
    auto set = DocValuesLongHashSet::create({});
    ASSERT_TRUE(set.has_value());
    EXPECT_EQ(set.value().size(), 0U);
    EXPECT_FALSE(set.value().contains(0));
    // An empty set reports the extremes so that a caller can guide iteration.
    EXPECT_EQ(set.value().min_value(), std::numeric_limits<int64_t>::max());
    EXPECT_EQ(set.value().max_value(), std::numeric_limits<int64_t>::min());
}

TEST(DocValuesLongHashSetTest, RendersItsValues) {
    auto set = DocValuesLongHashSet::create(std::vector<int64_t>{1, 2});
    ASSERT_TRUE(set.has_value());
    const std::string rendered = set.value().to_string();
    EXPECT_EQ(rendered.front(), '[');
    EXPECT_EQ(rendered.back(), ']');
    EXPECT_NE(rendered.find('1'), std::string::npos);
    EXPECT_NE(rendered.find('2'), std::string::npos);
}
