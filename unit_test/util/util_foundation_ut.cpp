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

// `std::isnan` / `std::bit_cast` are not reachable through `import std;` in this
// toolchain, so the C headers are included in the global module fragment.
#include <cmath>
#include <cstdint>
#include <limits>

module lucene_core:ut.util_foundation;

import lucene_core;

using namespace lucene;

namespace {

BytesRef bytes_ref_of(std::string_view text) {
    return BytesRef::from_string(text);
}

} // namespace

TEST(NumericUtilsTest, DoubleSortableLongPreservesOrder) {
    const std::vector<double> ascending{-std::numeric_limits<double>::infinity(), -1e300, -1.5, -0.0,
                                        0.0, 1.5, 1e300, std::numeric_limits<double>::infinity(),
                                        std::numeric_limits<double>::quiet_NaN()};
    for (size_t i = 1; i < ascending.size(); ++i) {
        const int64_t previous = NumericUtils::double_to_sortable_long(ascending[i - 1]);
        const int64_t current = NumericUtils::double_to_sortable_long(ascending[i]);
        EXPECT_LT(previous, current) << "index " << i;
    }
    for (const double value : ascending) {
        if (std::isnan(value)) {
            continue;
        }
        EXPECT_EQ(NumericUtils::sortable_long_to_double(NumericUtils::double_to_sortable_long(value)), value);
    }
    // Every NaN normalizes to the same sortable value.
    EXPECT_EQ(NumericUtils::double_to_sortable_long(std::numeric_limits<double>::quiet_NaN()),
              NumericUtils::double_to_sortable_long(std::bit_cast<double>(0x7ff8'0000'0000'0001ULL)));
}

TEST(NumericUtilsTest, FloatSortableIntPreservesOrder) {
    const std::vector<float> ascending{-std::numeric_limits<float>::infinity(), -2.5F, -0.0F, 0.0F, 2.5F,
                                       std::numeric_limits<float>::infinity(),
                                       std::numeric_limits<float>::quiet_NaN()};
    for (size_t i = 1; i < ascending.size(); ++i) {
        EXPECT_LT(NumericUtils::float_to_sortable_int(ascending[i - 1]),
                  NumericUtils::float_to_sortable_int(ascending[i]))
            << "index " << i;
    }
    EXPECT_EQ(NumericUtils::sortable_int_to_float(NumericUtils::float_to_sortable_int(-2.5F)), -2.5F);
}

TEST(NumericUtilsTest, IntegerSortableBytesCompareLikeNumbers) {
    const std::vector<int32_t> values{std::numeric_limits<int32_t>::min(), -1000, -1, 0, 1, 1000,
                                      std::numeric_limits<int32_t>::max()};
    std::vector<std::vector<uint8_t>> encoded;
    for (const int32_t value : values) {
        std::vector<uint8_t> buffer(4);
        NumericUtils::int_to_sortable_bytes(value, buffer, 0);
        encoded.push_back(buffer);
        EXPECT_EQ(NumericUtils::sortable_bytes_to_int(buffer, 0), value);
    }
    for (size_t i = 1; i < encoded.size(); ++i) {
        EXPECT_LT(encoded[i - 1], encoded[i]) << "index " << i;
    }

    const std::vector<int64_t> longs{std::numeric_limits<int64_t>::min(), -1, 0, 1,
                                     std::numeric_limits<int64_t>::max()};
    for (const int64_t value : longs) {
        std::vector<uint8_t> buffer(10);
        NumericUtils::long_to_sortable_bytes(value, buffer, 2);
        EXPECT_EQ(NumericUtils::sortable_bytes_to_long(buffer, 2), value);
    }
}

TEST(NumericUtilsTest, AddAndSubtractDimensions) {
    const std::vector<uint8_t> a{0x01, 0x00, 0x00, 0x10};
    const std::vector<uint8_t> b{0x00, 0x02, 0x00, 0x0F};
    std::vector<uint8_t> result(2);

    // Dimension 0 is `a[0..2] = 0x0100`, `b[0..2] = 0x0002`.
    ASSERT_TRUE(NumericUtils::subtract(2, 0, a, b, result).has_value());
    EXPECT_EQ(result, (std::vector<uint8_t>{0x00, 0xFE}));

    std::vector<uint8_t> smaller{0x00, 0x01};
    auto failed = NumericUtils::subtract(2, 0, smaller, b, result);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().kind(), ErrorKind::kIllegalArgument);

    // Dimension 1 is `a[2..4] = 0x0010`, `b[2..4] = 0x000F`.
    std::vector<uint8_t> sum(2);
    ASSERT_TRUE(NumericUtils::add(2, 1, a, b, sum).has_value());
    EXPECT_EQ(sum, (std::vector<uint8_t>{0x00, 0x1F}));

    std::vector<uint8_t> overflowing{0xFF, 0xFF};
    auto overflow = NumericUtils::add(2, 0, overflowing, smaller, sum);
    ASSERT_FALSE(overflow.has_value());
    EXPECT_EQ(overflow.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(BitsTest, MatchAllAndMatchNo) {
    MatchAllBits all(4);
    EXPECT_EQ(all.length(), 4u);
    for (size_t i = 0; i < 4; ++i) {
        ASSERT_TRUE(all.get(i).has_value());
        EXPECT_TRUE(all.get(i).value());
    }
    EXPECT_FALSE(all.get(4).has_value());

    MatchNoBits none(4);
    EXPECT_EQ(none.length(), 4u);
    ASSERT_TRUE(none.get(0).has_value());
    EXPECT_FALSE(none.get(0).value());
    EXPECT_FALSE(none.get(4).has_value());
}

TEST(NaturalBytesRefComparatorTest, OrdersUnsignedLexicographically) {
    NaturalBytesRefComparator comparator;
    const auto abc = bytes_ref_of("abc");
    const auto abd = bytes_ref_of("abd");
    const auto ab = bytes_ref_of("ab");

    EXPECT_LT(comparator.compare(abc, abd), 0);
    EXPECT_GT(comparator.compare(abd, abc), 0);
    EXPECT_EQ(comparator.compare(abc, abc), 0);
    // A prefix sorts first.
    EXPECT_LT(comparator.compare(ab, abc), 0);
    EXPECT_GT(comparator.compare(abc, ab), 0);

    // Bytes compare as unsigned, so 0xFF sorts after 'a' (0x61).
    std::vector<uint8_t> high{0xFF};
    const BytesRef high_ref(high);
    EXPECT_GT(comparator.compare(high_ref, bytes_ref_of("a")), 0);

    EXPECT_EQ(comparator.byte_at(abc, 0), 'a');
    EXPECT_EQ(comparator.byte_at(abc, 3), -1);
    // Byte 2 differs ('c' vs 'd'); from byte 3 on both are exhausted.
    EXPECT_LT(comparator.compare_with_offset(abc, abd, 2), 0);
    EXPECT_EQ(comparator.compare_with_offset(abc, abd, 3), 0);
    EXPECT_EQ(comparator.compared_bytes_count(), static_cast<size_t>(std::numeric_limits<int32_t>::max()));
}

TEST(CollectionUtilTest, IntroAndTimSort) {
    std::vector<int32_t> values{5, 3, 9, 1, 7};
    CollectionUtil::intro_sort(std::span<int32_t>(values));
    EXPECT_EQ(values, (std::vector<int32_t>{1, 3, 5, 7, 9}));

    std::vector<int32_t> descending{5, 3, 9, 1, 7};
    CollectionUtil::intro_sort(std::span<int32_t>(descending), [](int32_t a, int32_t b) { return b - a; });
    EXPECT_EQ(descending, (std::vector<int32_t>{9, 7, 5, 3, 1}));

    // `tim_sort` must be stable: equal keys keep their insertion order.
    struct Item {
        int32_t key;
        int32_t order;
    };
    std::vector<Item> items{{1, 0}, {0, 1}, {1, 2}, {0, 3}, {1, 4}};
    CollectionUtil::tim_sort(std::span<Item>(items),
                             [](const Item &a, const Item &b) { return a.key < b.key ? -1 : (a.key > b.key ? 1 : 0); });
    const std::vector<int32_t> orders{1, 3, 0, 2, 4};
    for (size_t i = 0; i < items.size(); ++i) {
        EXPECT_EQ(items[i].order, orders[i]) << "index " << i;
    }

    auto map = CollectionUtil::new_hashmap<std::string, int32_t>(100);
    map.emplace("a", 1);
    EXPECT_EQ(map.size(), 1u);
}
