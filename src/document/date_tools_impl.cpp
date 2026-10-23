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

module lucene_core:date_tools.impl;

import :date_tools;

import std;

import :error;
import :types;

namespace lucene {

namespace {

constexpr int64_t kMillisPerSecond = 1000;
constexpr int64_t kMillisPerMinute = 60 * kMillisPerSecond;
constexpr int64_t kMillisPerHour = 60 * kMillisPerMinute;
constexpr int64_t kMillisPerDay = 24 * kMillisPerHour;

/// A calendar date, in UTC.
struct CivilDate {
    int32_t year{1970};
    int32_t month{1};
    int32_t day{1};
};

/// Days since 1970-01-01 of `date` (Howard Hinnant's civil calendar algorithm).
[[nodiscard]] int64_t days_from_civil(CivilDate date) noexcept {
    int64_t year = date.year;
    const int64_t month = date.month;
    const int64_t day = date.day;
    year -= month <= 2 ? 1 : 0;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const int64_t year_of_era = year - era * 400;
    const int64_t day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int64_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

/// The calendar date `days` days after 1970-01-01.
[[nodiscard]] CivilDate civil_from_days(int64_t days) noexcept {
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const int64_t day_of_era = days - era * 146097;
    const int64_t year_of_era =
            (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const int64_t year = year_of_era + era * 400;
    const int64_t day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const int64_t mp = (5 * day_of_year + 2) / 153;
    const int64_t day = day_of_year - (153 * mp + 2) / 5 + 1;
    const int64_t month = mp + (mp < 10 ? 3 : -9);
    return CivilDate{static_cast<int32_t>(year + (month <= 2 ? 1 : 0)), static_cast<int32_t>(month),
                     static_cast<int32_t>(day)};
}

[[nodiscard]] bool is_leap_year(int32_t year) noexcept {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

[[nodiscard]] int32_t days_in_month(int32_t year, int32_t month) noexcept {
    static constexpr std::array<int32_t, 12> kDays{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return kDays[static_cast<size_t>(month - 1)];
}

[[nodiscard]] LuceneError invalid_date_string(std::string_view text) {
    return LuceneError::illegal_argument("Input is not a valid date string: " + std::string(text));
}

/// Parses `text[start, end)` as a non-negative integer.
[[nodiscard]] Result<int32_t> parse_digits(std::string_view text, size_t start, size_t end) {
    int32_t value = 0;
    for (size_t i = start; i < end; ++i) {
        const char ch = text[i];
        if (ch < '0' || ch > '9') {
            return err(invalid_date_string(text));
        }
        value = value * 10 + (ch - '0');
    }
    return value;
}

} // namespace

size_t resolution_format_len(Resolution resolution) noexcept {
    switch (resolution) {
        case Resolution::Year:
            return 4;
        case Resolution::Month:
            return 6;
        case Resolution::Day:
            return 8;
        case Resolution::Hour:
            return 10;
        case Resolution::Minute:
            return 12;
        case Resolution::Second:
            return 14;
        case Resolution::Millisecond:
            return 17;
    }
    return 0;
}

std::string_view resolution_to_string(Resolution resolution) noexcept {
    switch (resolution) {
        case Resolution::Year:
            return "year";
        case Resolution::Month:
            return "month";
        case Resolution::Day:
            return "day";
        case Resolution::Hour:
            return "hour";
        case Resolution::Minute:
            return "minute";
        case Resolution::Second:
            return "second";
        case Resolution::Millisecond:
            return "millisecond";
    }
    return "";
}

std::array<Resolution, 7> resolution_values() noexcept {
    return {Resolution::Year,  Resolution::Month, Resolution::Day,   Resolution::Hour,
            Resolution::Minute, Resolution::Second, Resolution::Millisecond};
}

Result<int64_t> DateTools::round(int64_t time, Resolution resolution) {
    if (resolution == Resolution::Millisecond) {
        return time;
    }
    // Floor division, so times before the epoch round down as well.
    int64_t days = time / kMillisPerDay;
    int64_t millis_of_day = time % kMillisPerDay;
    if (millis_of_day < 0) {
        millis_of_day += kMillisPerDay;
        --days;
    }
    CivilDate date = civil_from_days(days);
    int64_t hour = millis_of_day / kMillisPerHour;
    int64_t minute = (millis_of_day % kMillisPerHour) / kMillisPerMinute;
    int64_t second = (millis_of_day % kMillisPerMinute) / kMillisPerSecond;

    switch (resolution) {
        case Resolution::Year:
            date.month = 1;
            date.day = 1;
            hour = 0;
            minute = 0;
            second = 0;
            break;
        case Resolution::Month:
            date.day = 1;
            hour = 0;
            minute = 0;
            second = 0;
            break;
        case Resolution::Day:
            hour = 0;
            minute = 0;
            second = 0;
            break;
        case Resolution::Hour:
            minute = 0;
            second = 0;
            break;
        case Resolution::Minute:
            second = 0;
            break;
        case Resolution::Second:
        case Resolution::Millisecond:
            break;
    }
    return days_from_civil(date) * kMillisPerDay + hour * kMillisPerHour + minute * kMillisPerMinute +
           second * kMillisPerSecond;
}

Result<std::string> DateTools::time_to_string(int64_t time, Resolution resolution) {
    auto rounded = round(time, resolution);
    if (!rounded.has_value()) {
        return std::unexpected(std::move(rounded.error()));
    }
    int64_t days = rounded.value() / kMillisPerDay;
    int64_t millis_of_day = rounded.value() % kMillisPerDay;
    if (millis_of_day < 0) {
        millis_of_day += kMillisPerDay;
        --days;
    }
    const CivilDate date = civil_from_days(days);
    const int64_t hour = millis_of_day / kMillisPerHour;
    const int64_t minute = (millis_of_day % kMillisPerHour) / kMillisPerMinute;
    const int64_t second = (millis_of_day % kMillisPerMinute) / kMillisPerSecond;
    const int64_t millisecond = millis_of_day % kMillisPerSecond;

    switch (resolution) {
        case Resolution::Year:
            return std::format("{:04}", date.year);
        case Resolution::Month:
            return std::format("{:04}{:02}", date.year, date.month);
        case Resolution::Day:
            return std::format("{:04}{:02}{:02}", date.year, date.month, date.day);
        case Resolution::Hour:
            return std::format("{:04}{:02}{:02}{:02}", date.year, date.month, date.day, hour);
        case Resolution::Minute:
            return std::format("{:04}{:02}{:02}{:02}{:02}", date.year, date.month, date.day, hour, minute);
        case Resolution::Second:
            return std::format("{:04}{:02}{:02}{:02}{:02}{:02}", date.year, date.month, date.day, hour, minute,
                               second);
        case Resolution::Millisecond:
            return std::format("{:04}{:02}{:02}{:02}{:02}{:02}{:03}", date.year, date.month, date.day, hour, minute,
                               second, millisecond);
    }
    return err(LuceneError::illegal_argument("unknown resolution"));
}

Result<int64_t> DateTools::string_to_time(std::string_view date_string) {
    switch (date_string.size()) {
        case 4:
        case 6:
        case 8:
        case 10:
        case 12:
        case 14:
        case 17:
            break;
        default:
            return err(invalid_date_string(date_string));
    }
    for (const char ch : date_string) {
        if (ch < '0' || ch > '9') {
            return err(invalid_date_string(date_string));
        }
    }

    auto year = parse_digits(date_string, 0, 4);
    if (!year.has_value()) {
        return std::unexpected(std::move(year.error()));
    }
    const auto parse_or_default = [&date_string](size_t start, size_t end,
                                                 int32_t fallback) -> Result<int32_t> {
        if (date_string.size() >= end) {
            return parse_digits(date_string, start, end);
        }
        return fallback;
    };
    auto month = parse_or_default(4, 6, 1);
    auto day = parse_or_default(6, 8, 1);
    auto hour = parse_or_default(8, 10, 0);
    auto minute = parse_or_default(10, 12, 0);
    auto second = parse_or_default(12, 14, 0);
    auto millisecond = parse_or_default(14, 17, 0);
    for (const auto *parsed : {&month, &day, &hour, &minute, &second, &millisecond}) {
        if (!parsed->has_value()) {
            return std::unexpected(std::move(parsed->error()));
        }
    }

    const int32_t y = year.value();
    const int32_t mo = month.value();
    const int32_t d = day.value();
    if (mo < 1 || mo > 12 || d < 1 || d > days_in_month(y, mo) || hour.value() > 23 || minute.value() > 59 ||
        second.value() > 59 || millisecond.value() > 999) {
        return err(invalid_date_string(date_string));
    }
    return days_from_civil(CivilDate{y, mo, d}) * kMillisPerDay + hour.value() * kMillisPerHour +
           minute.value() * kMillisPerMinute + second.value() * kMillisPerSecond + millisecond.value();
}

} // namespace lucene
