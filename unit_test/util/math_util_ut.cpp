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

module lucene_core:ut.math_util;

import lucene_core;

using namespace lucene;

TEST(MathUtilTest, Log) {
    EXPECT_EQ(MathUtil::log(0, 2).value(), 0);
    EXPECT_EQ(MathUtil::log(-10, 2).value(), 0);
    EXPECT_EQ(MathUtil::log(1, 2).value(), 0);
    EXPECT_EQ(MathUtil::log(2, 2).value(), 1);
    EXPECT_EQ(MathUtil::log(3, 2).value(), 1);
    EXPECT_EQ(MathUtil::log(1024, 2).value(), 10);
    EXPECT_EQ(MathUtil::log(1000, 10).value(), 3);
    EXPECT_EQ(MathUtil::log(999, 10).value(), 2);
    EXPECT_EQ(MathUtil::log(64, 8).value(), 2);

    auto invalid = MathUtil::log(10, 1);
    ASSERT_FALSE(invalid.has_value());
    EXPECT_EQ(invalid.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(MathUtilTest, Gcd) {
    EXPECT_EQ(MathUtil::gcd(12, 18), 6);
    EXPECT_EQ(MathUtil::gcd(0, 5), 5);
    EXPECT_EQ(MathUtil::gcd(5, 0), 5);
    EXPECT_EQ(MathUtil::gcd(-12, 18), 6);
    EXPECT_EQ(MathUtil::gcd(0, 0), 0);
    EXPECT_EQ(MathUtil::gcd(17, 5), 1);
    // Overflow corner case documented in the reference implementation: gcd(INT64_MIN, 0) == INT64_MIN.
    EXPECT_EQ(MathUtil::gcd(std::numeric_limits<int64_t>::min(), 0), std::numeric_limits<int64_t>::min());
}

TEST(MathUtilTest, HyperbolicInverses) {
    EXPECT_NEAR(MathUtil::asinh(0.0), 0.0, 1e-12);
    EXPECT_NEAR(MathUtil::asinh(1.0), 0.88137358701954302523, 1e-12);
    EXPECT_NEAR(MathUtil::asinh(-2.5), -1.64723114637109580295, 1e-12);
    EXPECT_NEAR(MathUtil::acosh(2.0), 1.3169578969248166, 1e-12);
    EXPECT_NEAR(MathUtil::atanh(0.5), 0.5493061443340548, 1e-12);
    // -0 keeps its sign.
    EXPECT_LT(std::bit_cast<int64_t>(MathUtil::asinh(-0.0)), 0);
    EXPECT_LT(std::bit_cast<int64_t>(MathUtil::atanh(-0.0)), 0);
}

TEST(MathUtilTest, SumErrorBounds) {
    EXPECT_EQ(MathUtil::sum_relative_error_bound(0), 0.0);
    EXPECT_EQ(MathUtil::sum_relative_error_bound(1), 0.0);
    EXPECT_NEAR(MathUtil::sum_relative_error_bound(2), std::bit_cast<double>(0x3CB0000000000000ull), 0.0);
    EXPECT_EQ(MathUtil::sum_upper_bound(10.0, 1), 10.0);
    EXPECT_EQ(MathUtil::sum_upper_bound(10.0, 2), 10.0);
    EXPECT_GT(MathUtil::sum_upper_bound(10.0, 100), 10.0);
}
