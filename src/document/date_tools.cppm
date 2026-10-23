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

export module lucene_core:date_tools;

import :types;
import std;

import :error;

namespace lucene {

/// The time granularity of a date string.
///
/// Port of `Resolution` (`core/document/date_tools.rs`).
export enum class Resolution : int32_t {
    Year = 0,
    Month = 1,
    Day = 2,
    Hour = 3,
    Minute = 4,
    Second = 5,
    Millisecond = 6,
};

/// The length of the date string format of `resolution`.
[[nodiscard]] size_t resolution_format_len(Resolution resolution) noexcept;

/// The lowercase name of `resolution` (`"year"`, `"month"`, ...).
[[nodiscard]] std::string_view resolution_to_string(Resolution resolution) noexcept;

/// Every resolution, from the coarsest to the finest.
[[nodiscard]] std::array<Resolution, 7> resolution_values() noexcept;

/// Converts dates to `yyyyMMddHHmmssSSS` strings, and back, in UTC.
///
/// Port of `DateTools` (`core/document/date_tools.rs`).  The strings sort
/// lexicographically in date order, which makes them usable as term values; the
/// coarser the resolution, the fewer terms a range query has to visit.
///
/// **Deviation:** the reference implementation takes `chrono::DateTime<Utc>`
/// values; the port works on milliseconds since the epoch and does its own UTC
/// calendar arithmetic, so no date/time library is needed.
export struct DateTools {
    /// `time_to_string` of the millisecond timestamp of `time`.
    [[nodiscard]] static Result<std::string> time_to_string(int64_t time, Resolution resolution);

    /// Converts a string produced by `time_to_string` back to milliseconds since
    /// January 1, 1970, 00:00:00 UTC.
    [[nodiscard]] static Result<int64_t> string_to_time(std::string_view date_string);

    /// Limits `time` to `resolution`: every field finer than the resolution is
    /// set to its lowest value.
    [[nodiscard]] static Result<int64_t> round(int64_t time, Resolution resolution);
};

} // namespace lucene
