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

module lucene_core:ut.ints_ref;

import lucene_core;

using namespace lucene;

TEST(IntsRefTest, ComparesLexicographically) {
    const IntsRef ab{std::vector<int32_t>{1, 2}};
    const IntsRef ac{std::vector<int32_t>{1, 3}};
    const IntsRef a{std::vector<int32_t>{1}};
    const IntsRef empty_is_first{std::vector<int32_t>{0, 9}};

    EXPECT_LT(ab.compare_to(ac), 0);
    EXPECT_GT(ac.compare_to(ab), 0);
    EXPECT_EQ(ab.compare_to(ab), 0);
    // A proper prefix sorts first.
    EXPECT_LT(a.compare_to(ab), 0);
    EXPECT_GT(ab.compare_to(a), 0);
    EXPECT_LT(empty_is_first.compare_to(ab), 0);
    EXPECT_TRUE(ab.equals(IntsRef{std::vector<int32_t>{1, 2}}));
    EXPECT_FALSE(ab.equals(a));
}

TEST(IntsRefTest, SlicesAndValidity) {
    IntsRef ref = IntsRef::from_slice({7, 8, 9, 10}, 1, 2);
    ASSERT_TRUE(ref.is_valid().has_value());
    ASSERT_EQ(ref.as_slice().size(), 2u);
    EXPECT_EQ(ref.as_slice()[0], 8);
    EXPECT_EQ(ref.as_slice()[1], 9);

    ref.as_mutable_slice()[1] = 42;
    EXPECT_EQ(ref.ints()[2], 42);

    IntsRef invalid = IntsRef::from_slice({1, 2}, 1, 5);
    EXPECT_FALSE(invalid.is_valid().has_value());
    EXPECT_EQ(ref.to_string(), "[8, 42]");

    const IntsRef copy = IntsRef::deep_copy_of(ref);
    EXPECT_EQ(copy.length(), 2u);
    EXPECT_EQ(copy.offset(), 0u);
    EXPECT_TRUE(copy.equals(ref));
}

TEST(IntsRefBuilderTest, AppendsGrowsAndCopies) {
    IntsRefBuilder builder;
    ASSERT_TRUE(builder.append(4).has_value());
    ASSERT_TRUE(builder.append(5).has_value());
    EXPECT_EQ(builder.length(), 2u);
    EXPECT_EQ(builder.int_at(1), 5);

    builder.set_int_at(0, 9);
    EXPECT_EQ(builder.int_at(0), 9);

    ASSERT_TRUE(builder.grow(4).has_value());
    EXPECT_EQ(builder.length(), 4u);
    EXPECT_EQ(builder.int_at(2), 0);

    const std::vector<int32_t> source{1, 2, 3};
    ASSERT_TRUE(builder.copy_ints(source, 0, source.size()).has_value());
    EXPECT_EQ(builder.length(), 3u);
    EXPECT_EQ(builder.to_ints_ref().to_string(), "[1, 2, 3]");

    builder.clear();
    EXPECT_EQ(builder.length(), 0u);

    const IntsRef other{std::vector<int32_t>{7, 8}};
    ASSERT_TRUE(builder.copy_ints_ref(other).has_value());
    EXPECT_TRUE(builder.to_ints_ref().equals(other));

    EXPECT_FALSE(builder.copy_ints(source, 2, 5).has_value());
}
