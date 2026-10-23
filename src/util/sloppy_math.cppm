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

export module lucene_core:sloppy_math;

import :types;
import std;

namespace lucene {

/// Floating-point math routines that trade precision for speed, with a bounded
/// error.
///
/// Port of `SloppyMath` (`core/util/sloppy_math.rs`).
///
/// `cos` and `asin` use lookup tables plus a fourth-order Taylor expansion, so
/// their results differ from `std::cos`/`std::asin` in the last bits; the geo
/// code depends on that difference staying stable, hence the tables are built
/// exactly the way the reference implementation builds them.  The haversine
/// helpers work on *sort keys* (non-negative doubles whose bit patterns sort in
/// the same order as the distances they encode), which is how the doc values
/// code compares distances without any division.
export struct SloppyMath {
    /// Mean earth axis in meters (WGS84).
    static constexpr double TO_METERS = 6'371'008.7714;

    /// The great-circle distance between two points, in meters.
    [[nodiscard]] static double haversin_meters(double lat1, double lon1, double lat2, double lon2);

    /// The distance in meters encoded by `sort_key`.
    [[nodiscard]] static double haversin_meters_from_sort_key(double sort_key);

    /// The distance between two points as a sortable key (low three bits cleared).
    [[nodiscard]] static double haversin_sort_key(double lat1, double lon1, double lat2, double lon2);

    /// The cosine, computed from a lookup table.
    [[nodiscard]] static double cos(double a);

    /// The arcsine, computed from a lookup table.
    [[nodiscard]] static double asin(double a);
};

} // namespace lucene
