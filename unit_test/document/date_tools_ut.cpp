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

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

module lucene_core:ut.date_tools;

import lucene_core;

using namespace lucene;

namespace {

/// 2004-09-21T13:50:11Z.
constexpr int64_t kReferenceTime = 1095774611000;

} // namespace

TEST(DateToolsTest, FormatsTimesAtEveryResolution) {
    EXPECT_EQ(DateTools::time_to_string(kReferenceTime, Resolution::Year).value(), "2004");
    EXPECT_EQ(DateTools::time_to_string(kReferenceTime, Resolution::Month).value(), "200409");
    EXPECT_EQ(DateTools::time_to_string(kReferenceTime, Resolution::Day).value(), "20040921");
    EXPECT_EQ(DateTools::time_to_string(kReferenceTime, Resolution::Hour).value(), "2004092113");
    EXPECT_EQ(DateTools::time_to_string(kReferenceTime, Resolution::Minute).value(), "200409211350");
    EXPECT_EQ(DateTools::time_to_string(kReferenceTime, Resolution::Second).value(), "20040921135011");
    EXPECT_EQ(DateTools::time_to_string(kReferenceTime, Resolution::Millisecond).value(), "20040921135011000");
    EXPECT_EQ(DateTools::time_to_string(0, Resolution::Millisecond).value(), "19700101000000000");
}

TEST(DateToolsTest, FormatsTimesBeforeTheEpoch) {
    EXPECT_EQ(DateTools::time_to_string(-1000, Resolution::Millisecond).value(), "19691231235959000");
    EXPECT_EQ(DateTools::time_to_string(-1, Resolution::Second).value(), "19691231235959");
    EXPECT_EQ(DateTools::time_to_string(-86400000, Resolution::Day).value(), "19691231");
}

TEST(DateToolsTest, RoundsTimesDown) {
    const auto second = DateTools::round(kReferenceTime + 999, Resolution::Second);
    ASSERT_TRUE(second.has_value()) << (second ? "" : second.error().to_string());
    EXPECT_EQ(second.value(), kReferenceTime);

    const auto minute = DateTools::round(kReferenceTime + 59999, Resolution::Minute);
    ASSERT_TRUE(minute.has_value());
    // 13:50:11.999 rounds down to 13:51 only when the seconds carry over.
    EXPECT_EQ(DateTools::time_to_string(minute.value(), Resolution::Minute).value(), "200409211351");

    const auto hour = DateTools::round(kReferenceTime, Resolution::Hour);
    ASSERT_TRUE(hour.has_value());
    EXPECT_EQ(DateTools::time_to_string(hour.value(), Resolution::Hour).value(), "2004092113");

    const auto day = DateTools::round(kReferenceTime, Resolution::Day);
    ASSERT_TRUE(day.has_value());
    EXPECT_EQ(DateTools::time_to_string(day.value(), Resolution::Day).value(), "20040921");

    const auto month = DateTools::round(kReferenceTime, Resolution::Month);
    ASSERT_TRUE(month.has_value());
    EXPECT_EQ(month.value(), 1093996800000);

    const auto year = DateTools::round(kReferenceTime, Resolution::Year);
    ASSERT_TRUE(year.has_value());
    EXPECT_EQ(DateTools::time_to_string(year.value(), Resolution::Year).value(), "2004");

    const auto exact = DateTools::round(kReferenceTime, Resolution::Millisecond);
    ASSERT_TRUE(exact.has_value());
    EXPECT_EQ(exact.value(), kReferenceTime);
}

TEST(DateToolsTest, ParsesEveryStringLength) {
    EXPECT_EQ(DateTools::string_to_time("2004").value(), 1072915200000);
    EXPECT_EQ(DateTools::string_to_time("200409").value(), 1093996800000);
    EXPECT_EQ(DateTools::string_to_time("20040921").value(), 1095724800000);
    EXPECT_EQ(DateTools::string_to_time("2004092113").value(), 1095771600000);
    EXPECT_EQ(DateTools::string_to_time("200409211350").value(), 1095774600000);
    EXPECT_EQ(DateTools::string_to_time("20040921135011").value(), kReferenceTime);
    EXPECT_EQ(DateTools::string_to_time("20040921135011000").value(), kReferenceTime);
}

TEST(DateToolsTest, RoundTripsThroughEveryResolution) {
    for (const Resolution resolution : resolution_values()) {
        auto rounded = DateTools::round(kReferenceTime, resolution);
        ASSERT_TRUE(rounded.has_value());
        auto text = DateTools::time_to_string(rounded.value(), resolution);
        ASSERT_TRUE(text.has_value());
        EXPECT_EQ(text.value().size(), resolution_format_len(resolution));
        auto parsed = DateTools::string_to_time(text.value());
        ASSERT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().to_string());
        EXPECT_EQ(parsed.value(), rounded.value());
    }
}

TEST(DateToolsTest, RejectsInvalidDateStrings) {
    const std::array<std::string_view, 6> invalid{"200", "2004a", "200413", "20040230", "2004092113501", "20041332"};
    for (const std::string_view text : invalid) {
        auto parsed = DateTools::string_to_time(text);
        ASSERT_FALSE(parsed.has_value()) << text;
        EXPECT_EQ(parsed.error().kind(), ErrorKind::kIllegalArgument);
    }

    // February 29th is valid in a leap year only.
    EXPECT_TRUE(DateTools::string_to_time("20040229").has_value());
    EXPECT_FALSE(DateTools::string_to_time("20030229").has_value());
    EXPECT_TRUE(DateTools::string_to_time("20000229").has_value());
}

TEST(DateToolsTest, NamesAndOrdersTheResolutions) {
    EXPECT_EQ(resolution_to_string(Resolution::Year), "year");
    EXPECT_EQ(resolution_to_string(Resolution::Millisecond), "millisecond");
    const auto values = resolution_values();
    EXPECT_EQ(values.front(), Resolution::Year);
    EXPECT_EQ(values.back(), Resolution::Millisecond);
    for (size_t i = 1; i < values.size(); ++i) {
        EXPECT_LT(resolution_format_len(values[i - 1]), resolution_format_len(values[i]));
    }
}
