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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

module lucene_core:ut.sort_field_provider;

import lucene_core;

using namespace lucene;

namespace {

/// Writes `field` the way the segment metadata does and returns the bytes.
std::vector<uint8_t> write_field(const SortField &field) {
    ByteArrayOutput out("sort-field");
    EXPECT_TRUE(write_sort_field(field, out).has_value());
    return std::vector<uint8_t>(out.bytes().begin(), out.bytes().end());
}

/// Reads back what [`write_field`] wrote.
SortField read_field(std::string_view provider_name, const std::vector<uint8_t> &bytes) {
    ByteArrayDataInput in(bytes);
    auto field = read_sort_field(provider_name, in);
    EXPECT_TRUE(field.has_value()) << (field ? "" : field.error().to_string());
    return field.has_value() ? field.value() : SortField::score().value();
}

} // namespace

TEST(SortFieldProviderTest, NamesTheProviderOfEachType) {
    EXPECT_EQ(sort_field_provider_name(SortField::with_reverse("f", SortFieldType::kString, false).value()).value(),
              SORT_FIELD_PROVIDER);
    EXPECT_EQ(sort_field_provider_name(SortField::with_reverse("f", SortFieldType::kInt, false).value()).value(),
              SORT_FIELD_PROVIDER);
    EXPECT_EQ(sort_field_provider_name(SortField::with_reverse("f", SortFieldType::kLong, false).value()).value(),
              SORT_FIELD_PROVIDER);
    EXPECT_EQ(sort_field_provider_name(SortField::with_reverse("f", SortFieldType::kFloat, false).value()).value(),
              SORT_FIELD_PROVIDER);
    EXPECT_EQ(sort_field_provider_name(SortField::with_reverse("f", SortFieldType::kDouble, false).value()).value(),
              SORT_FIELD_PROVIDER);
    EXPECT_EQ(sort_field_provider_name(
                      SortField::with_selector("f", SortFieldType::kSortedNumeric, SortFieldSelector::kMin).value())
                      .value(),
              SORTED_NUMERIC_SORT_FIELD_PROVIDER);
    EXPECT_EQ(sort_field_provider_name(SortField::with_selector("f", SortFieldType::kSortedNumericDouble,
                                                                SortFieldSelector::kMin)
                                               .value())
                      .value(),
              SORTED_NUMERIC_SORT_FIELD_PROVIDER);
    EXPECT_EQ(sort_field_provider_name(
                      SortField::with_selector("f", SortFieldType::kSortedSet, SortFieldSelector::kMax).value())
                      .value(),
              SORTED_SET_SORT_FIELD_PROVIDER);
}

TEST(SortFieldProviderTest, RoundTripsTheScalarTypes) {
    const std::vector<std::pair<SortFieldType, bool>> cases = {
            {SortFieldType::kString, false}, {SortFieldType::kString, true},
            {SortFieldType::kInt, false},    {SortFieldType::kInt, true},
            {SortFieldType::kFloat, true},   {SortFieldType::kLong, false},
            {SortFieldType::kDouble, true},
    };
    for (const auto &[type, reverse] : cases) {
        auto field = SortField::with_reverse("field", type, reverse);
        ASSERT_TRUE(field.has_value());
        const auto bytes = write_field(field.value());
        const SortField read = read_field(SORT_FIELD_PROVIDER, bytes);
        EXPECT_TRUE(read.equals(field.value())) << read.to_string().value();
        EXPECT_EQ(read.field().value(), "field");
        EXPECT_EQ(read.type(), type);
        EXPECT_EQ(read.reverse(), reverse);
    }
}

TEST(SortFieldProviderTest, RoundTripsTheMultiValuedTypes) {
    auto numeric = SortField::with_reverse_selector("num", SortFieldType::kSortedNumeric, SortFieldSelector::kMax, true);
    ASSERT_TRUE(numeric.has_value());
    auto read_numeric = read_field(SORTED_NUMERIC_SORT_FIELD_PROVIDER, write_field(numeric.value()));
    EXPECT_TRUE(read_numeric.equals(numeric.value())) << read_numeric.to_string().value();
    EXPECT_EQ(read_numeric.type(), SortFieldType::kSortedNumeric);
    EXPECT_EQ(read_numeric.selector(), SortFieldSelector::kMax);
    EXPECT_TRUE(read_numeric.reverse());

    auto numeric_double = SortField::with_reverse_selector("num2", SortFieldType::kSortedNumericDouble,
                                                          SortFieldSelector::kMin, false);
    ASSERT_TRUE(numeric_double.has_value());
    auto read_double = read_field(SORTED_NUMERIC_SORT_FIELD_PROVIDER, write_field(numeric_double.value()));
    EXPECT_TRUE(read_double.equals(numeric_double.value())) << read_double.to_string().value();
    EXPECT_EQ(read_double.type(), SortFieldType::kSortedNumericDouble);

    auto set = SortField::with_reverse_selector("tag", SortFieldType::kSortedSet, SortFieldSelector::kMax, true);
    ASSERT_TRUE(set.has_value());
    auto read_set = read_field(SORTED_SET_SORT_FIELD_PROVIDER, write_field(set.value()));
    EXPECT_TRUE(read_set.equals(set.value())) << read_set.to_string().value();
    EXPECT_EQ(read_set.type(), SortFieldType::kSortedSet);
    EXPECT_EQ(read_set.selector(), SortFieldSelector::kMax);
    EXPECT_TRUE(read_set.reverse());
}

TEST(SortFieldProviderTest, RejectsTheScoreAndDocTypes) {
    auto score = SortField::score();
    ASSERT_TRUE(score.has_value());
    ByteArrayOutput out("score");
    auto written = write_sort_field(score.value(), out);
    ASSERT_FALSE(written.has_value());
    EXPECT_EQ(written.error().kind(), ErrorKind::kIllegalArgument);

    auto doc = SortField::doc();
    ASSERT_TRUE(doc.has_value());
    auto written_doc = write_sort_field(doc.value(), out);
    ASSERT_FALSE(written_doc.has_value());
    EXPECT_EQ(written_doc.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(SortFieldProviderTest, RejectsAnUnknownProviderName) {
    const auto bytes = write_field(SortField::with_reverse("f", SortFieldType::kInt, false).value());
    ByteArrayDataInput in(bytes);
    auto field = read_sort_field("NoSuchProvider", in);
    ASSERT_FALSE(field.has_value());
    EXPECT_EQ(field.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(SortFieldProviderTest, ReadsAMissingValueWithoutKeepingIt) {
    // field name, type, reverse, "has missing value", value.
    ByteArrayOutput out("missing");
    ASSERT_TRUE(out.write_string("num").has_value());
    ASSERT_TRUE(out.write_string("INT").has_value());
    ASSERT_TRUE(out.write_int(1).has_value());
    ASSERT_TRUE(out.write_int(1).has_value());
    ASSERT_TRUE(out.write_int(42).has_value());
    const std::vector<uint8_t> bytes(out.bytes().begin(), out.bytes().end());

    const SortField read = read_field(SORT_FIELD_PROVIDER, bytes);
    EXPECT_EQ(read.field().value(), "num");
    EXPECT_EQ(read.type(), SortFieldType::kInt);
    EXPECT_TRUE(read.reverse());
}

TEST(SortFieldProviderTest, MapsTheValueTypeOfASortedNumericSort) {
    // "INT" values of a sorted numeric sort become the port's integral sort.
    ByteArrayOutput int_out("sn-int");
    ASSERT_TRUE(int_out.write_string("num").has_value());
    ASSERT_TRUE(int_out.write_string("INT").has_value());
    ASSERT_TRUE(int_out.write_int(0).has_value());
    ASSERT_TRUE(int_out.write_int(1).has_value());
    ASSERT_TRUE(int_out.write_int(0).has_value());
    const auto int_bytes = std::vector<uint8_t>(int_out.bytes().begin(), int_out.bytes().end());
    const SortField int_field = read_field(SORTED_NUMERIC_SORT_FIELD_PROVIDER, int_bytes);
    EXPECT_EQ(int_field.type(), SortFieldType::kSortedNumeric);
    EXPECT_EQ(int_field.selector(), SortFieldSelector::kMax);

    // "DOUBLE" values become the port's floating point sort.
    ByteArrayOutput double_out("sn-double");
    ASSERT_TRUE(double_out.write_string("num").has_value());
    ASSERT_TRUE(double_out.write_string("DOUBLE").has_value());
    ASSERT_TRUE(double_out.write_int(1).has_value());
    ASSERT_TRUE(double_out.write_int(0).has_value());
    ASSERT_TRUE(double_out.write_int(0).has_value());
    const auto double_bytes = std::vector<uint8_t>(double_out.bytes().begin(), double_out.bytes().end());
    const SortField double_field = read_field(SORTED_NUMERIC_SORT_FIELD_PROVIDER, double_bytes);
    EXPECT_EQ(double_field.type(), SortFieldType::kSortedNumericDouble);

    // A string is not a numeric sort.
    ByteArrayOutput string_out("sn-string");
    ASSERT_TRUE(string_out.write_string("num").has_value());
    ASSERT_TRUE(string_out.write_string("STRING").has_value());
    ASSERT_TRUE(string_out.write_int(0).has_value());
    ASSERT_TRUE(string_out.write_int(0).has_value());
    ASSERT_TRUE(string_out.write_int(0).has_value());
    const auto string_bytes = std::vector<uint8_t>(string_out.bytes().begin(), string_out.bytes().end());
    ByteArrayDataInput string_in(string_bytes);
    auto bad = read_sort_field(SORTED_NUMERIC_SORT_FIELD_PROVIDER, string_in);
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(SortFieldProviderTest, RejectsAnUnknownTypeOrSelector) {
    ByteArrayOutput type_out("bad-type");
    ASSERT_TRUE(type_out.write_string("f").has_value());
    ASSERT_TRUE(type_out.write_string("Galactic").has_value());
    const auto type_bytes = std::vector<uint8_t>(type_out.bytes().begin(), type_out.bytes().end());
    ByteArrayDataInput type_in(type_bytes);
    auto bad_type = read_sort_field(SORT_FIELD_PROVIDER, type_in);
    ASSERT_FALSE(bad_type.has_value());
    EXPECT_EQ(bad_type.error().kind(), ErrorKind::kIllegalArgument);

    ByteArrayOutput selector_out("bad-selector");
    ASSERT_TRUE(selector_out.write_string("tag").has_value());
    ASSERT_TRUE(selector_out.write_int(0).has_value());
    ASSERT_TRUE(selector_out.write_int(7).has_value());
    const auto selector_bytes = std::vector<uint8_t>(selector_out.bytes().begin(), selector_out.bytes().end());
    ByteArrayDataInput selector_in(selector_bytes);
    auto bad_selector = read_sort_field(SORTED_SET_SORT_FIELD_PROVIDER, selector_in);
    ASSERT_FALSE(bad_selector.has_value());
    EXPECT_EQ(bad_selector.error().kind(), ErrorKind::kIllegalArgument);
}
