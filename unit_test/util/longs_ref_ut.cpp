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
#include <vector>

module lucene_core:ut.longs_ref;

import lucene_core;

using namespace lucene;

TEST(LongsRefTest, SlicesAndCopies) {
    LongsRef ref({10, 20, 30, 40});
    EXPECT_EQ(ref.length(), 4U);
    EXPECT_EQ(ref.offset(), 0U);
    EXPECT_EQ(ref.as_slice().size(), 4U);

    auto view = LongsRef::from_slice({10, 20, 30, 40}, 1, 2);
    EXPECT_EQ(view.offset(), 1U);
    EXPECT_EQ(view.length(), 2U);
    ASSERT_EQ(view.as_slice().size(), 2U);
    EXPECT_EQ(view.as_slice()[0], 20);
    EXPECT_EQ(view.as_slice()[1], 30);
    EXPECT_TRUE(view.is_valid().has_value());

    auto copy = LongsRef::deep_copy_of(view);
    EXPECT_EQ(copy.length(), 2U);
    EXPECT_EQ(copy.as_slice()[1], 30);
    EXPECT_EQ(copy.to_string(), "[20 30]");

    // A view that runs past the end is reported, and slicing it is clamped.
    auto broken = LongsRef::from_slice({1, 2}, 1, 5);
    EXPECT_FALSE(broken.is_valid().has_value());
    EXPECT_EQ(broken.as_slice().size(), 1U);
}

TEST(LongsRefTest, ComparesLexicographically) {
    LongsRef one({1, 2, 3});
    LongsRef other({1, 2, 4});
    EXPECT_LT(one.compare_to(other), 0);
    EXPECT_GT(other.compare_to(one), 0);
    EXPECT_EQ(one.compare_to(one), 0);
    EXPECT_FALSE(one.equals(other));

    // A prefix sorts first.
    LongsRef prefix({1, 2});
    EXPECT_LT(prefix.compare_to(one), 0);
    EXPECT_GT(one.compare_to(prefix), 0);

    // Element by element, not by packed value: (1, 5) < (2, 0).
    LongsRef low({1, 5});
    LongsRef high({2, 0});
    EXPECT_LT(LongsRefComparator::compare(low, high), 0);

    // Negative values compare by signed order.
    LongsRef negative({-1});
    LongsRef positive({1});
    EXPECT_LT(negative.compare_to(positive), 0);
}
