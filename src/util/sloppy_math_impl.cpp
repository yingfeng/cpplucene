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

module lucene_core:sloppy_math.impl;

import :sloppy_math;

import std;

import :core_helper;

namespace lucene {

namespace {

constexpr double ONE_DIV_F2 = 1.0 / 2.0;
constexpr double ONE_DIV_F3 = 1.0 / 6.0;
constexpr double ONE_DIV_F4 = 1.0 / 24.0;

/// `asin` uses the Taylor expansion of `asin(x)` around `pi/2` once the table
/// runs out, hence the two-part `pi/2` and the polynomial constants.
constexpr double PIO2_HI = std::bit_cast<double>(uint64_t{0x3FF921FB54400000ULL});
constexpr double PIO2_LO = std::bit_cast<double>(uint64_t{0x3DD0B4611A626331ULL});
constexpr double TWOPI_HI = 4.0 * PIO2_HI;
constexpr double TWOPI_LO = 4.0 * PIO2_LO;

constexpr size_t SIN_COS_TABS_SIZE = (1U << 11) + 1;
constexpr double SIN_COS_DELTA_HI = TWOPI_HI / static_cast<double>(SIN_COS_TABS_SIZE - 1);
constexpr double SIN_COS_DELTA_LO = TWOPI_LO / static_cast<double>(SIN_COS_TABS_SIZE - 1);
constexpr double SIN_COS_INDEXER = 1.0 / (SIN_COS_DELTA_HI + SIN_COS_DELTA_LO);

constexpr double SIN_COS_MAX_VALUE_FOR_INT_MODULO =
        ((static_cast<double>(std::numeric_limits<int32_t>::max() >> 9)) / SIN_COS_INDEXER) * 0.99;

constexpr double ASIN_MAX_VALUE_FOR_TABS = 0.9563047559630354;
constexpr size_t ASIN_TABS_SIZE = (1U << 13) + 1;
constexpr double ASIN_DELTA = ASIN_MAX_VALUE_FOR_TABS / static_cast<double>(ASIN_TABS_SIZE - 1);
constexpr double ASIN_INDEXER = 1.0 / ASIN_DELTA;

constexpr double ASIN_PIO2_HI = std::bit_cast<double>(uint64_t{0x3FF921FB54442D18ULL});
constexpr double ASIN_PIO2_LO = std::bit_cast<double>(uint64_t{0x3C91A62633145C07ULL});
constexpr double ASIN_PS0 = std::bit_cast<double>(uint64_t{0x3FC5555555555555ULL});
constexpr double ASIN_PS1 = std::bit_cast<double>(uint64_t{0xBFD4D61203EB6F7DULL});
constexpr double ASIN_PS2 = std::bit_cast<double>(uint64_t{0x3FC9C1550E884455ULL});
constexpr double ASIN_PS3 = std::bit_cast<double>(uint64_t{0xBFA48228B5688F3BULL});
constexpr double ASIN_PS4 = std::bit_cast<double>(uint64_t{0x3F49EFE07501B288ULL});
constexpr double ASIN_PS5 = std::bit_cast<double>(uint64_t{0x3F023DE10DFDF709ULL});
constexpr double ASIN_QS1 = std::bit_cast<double>(uint64_t{0xC0033A271C8A2D4BULL});
constexpr double ASIN_QS2 = std::bit_cast<double>(uint64_t{0x40002AE59C598AC8ULL});
constexpr double ASIN_QS3 = std::bit_cast<double>(uint64_t{0xBFE6066C1B8D0159ULL});
constexpr double ASIN_QS4 = std::bit_cast<double>(uint64_t{0x3FB3B8C5B12E9282ULL});

/// `[cos table, sin table]`, built lazily on first use.
struct SinCosTables {
    std::array<double, SIN_COS_TABS_SIZE> cos_tab{};
    std::array<double, SIN_COS_TABS_SIZE> sin_tab{};

    SinCosTables() {
        const size_t sin_cos_pi_index = (SIN_COS_TABS_SIZE - 1) / 2;
        const size_t sin_cos_pi_mul_0_5_index = sin_cos_pi_index / 2;
        const size_t sin_cos_pi_mul_1_5_index = 3 * sin_cos_pi_index / 2;
        const size_t sin_cos_pi_mul_2_index = 2 * sin_cos_pi_index;

        for (size_t i = 0; i < SIN_COS_TABS_SIZE; ++i) {
            const auto index = static_cast<double>(i);
            const double angle = index * SIN_COS_DELTA_HI + index * SIN_COS_DELTA_LO;
            double cos_angle = std::cos(angle);
            if (i == sin_cos_pi_mul_0_5_index || i == sin_cos_pi_mul_1_5_index) {
                cos_angle = 0.0;
            }
            cos_tab[i] = cos_angle;

            double sin_angle = std::sin(angle);
            if (i == sin_cos_pi_index || i == sin_cos_pi_mul_2_index) {
                sin_angle = 0.0;
            }
            sin_tab[i] = sin_angle;
        }
    }
};

/// `asin` table plus the first four derivatives divided by their factorials.
struct AsinTables {
    std::array<double, ASIN_TABS_SIZE> asin_tab{};
    std::array<double, ASIN_TABS_SIZE> asin_der1_div_f1_tab{};
    std::array<double, ASIN_TABS_SIZE> asin_der2_div_f2_tab{};
    std::array<double, ASIN_TABS_SIZE> asin_der3_div_f3_tab{};
    std::array<double, ASIN_TABS_SIZE> asin_der4_div_f4_tab{};

    AsinTables() {
        for (size_t i = 0; i < ASIN_TABS_SIZE; ++i) {
            const double x = static_cast<double>(i) * ASIN_DELTA;
            asin_tab[i] = std::asin(x);

            const double one_minus_x_sq_inv = 1.0 / (1.0 - x * x);
            const double one_minus_x_sq_inv0_5 = std::sqrt(one_minus_x_sq_inv);
            const double one_minus_x_sq_inv1_5 = one_minus_x_sq_inv0_5 * one_minus_x_sq_inv;
            const double one_minus_x_sq_inv2_5 = one_minus_x_sq_inv1_5 * one_minus_x_sq_inv;
            const double one_minus_x_sq_inv3_5 = one_minus_x_sq_inv2_5 * one_minus_x_sq_inv;

            asin_der1_div_f1_tab[i] = one_minus_x_sq_inv0_5;
            asin_der2_div_f2_tab[i] = (x * one_minus_x_sq_inv1_5) * ONE_DIV_F2;
            asin_der3_div_f3_tab[i] = ((1.0 + 2.0 * x * x) * one_minus_x_sq_inv2_5) * ONE_DIV_F3;
            asin_der4_div_f4_tab[i] =
                    ((5.0 + 2.0 * x * (2.0 + x * (5.0 - 2.0 * x))) * one_minus_x_sq_inv3_5) * ONE_DIV_F4;
        }
    }
};

[[nodiscard]] const SinCosTables &sin_cos_tables() {
    static const SinCosTables tables;
    return tables;
}

[[nodiscard]] const AsinTables &asin_tables() {
    static const AsinTables tables;
    return tables;
}

constexpr double DEGREES_TO_RADIANS = std::numbers::pi / 180.0;

} // namespace

double SloppyMath::haversin_meters(double lat1, double lon1, double lat2, double lon2) {
    return haversin_meters_from_sort_key(haversin_sort_key(lat1, lon1, lat2, lon2));
}

double SloppyMath::haversin_meters_from_sort_key(double sort_key) {
    const double value = CoreHelper::min_f64(1.0, std::sqrt(sort_key * 0.5));
    return TO_METERS * 2.0 * asin(value);
}

double SloppyMath::haversin_sort_key(double lat1, double lon1, double lat2, double lon2) {
    const double x1 = lat1 * DEGREES_TO_RADIANS;
    const double x2 = lat2 * DEGREES_TO_RADIANS;
    const double h1 = 1.0 - cos(x1 - x2);
    const double h2 = 1.0 - cos((lon1 - lon2) * DEGREES_TO_RADIANS);
    const double h = h1 + cos(x1) * cos(x2) * h2;
    return std::bit_cast<double>(std::bit_cast<uint64_t>(h) & 0xFFFFFFFFFFFFFFF8ULL);
}

double SloppyMath::cos(double a) {
    if (!std::isfinite(a)) {
        return std::cos(a);
    }
    if (a < 0.0) {
        a = -a;
    }
    if (a > SIN_COS_MAX_VALUE_FOR_INT_MODULO) {
        return std::cos(a);
    }

    size_t index = static_cast<size_t>(a * SIN_COS_INDEXER + 0.5);
    const double delta =
            (a - static_cast<double>(index) * SIN_COS_DELTA_HI) - static_cast<double>(index) * SIN_COS_DELTA_LO;
    index &= SIN_COS_TABS_SIZE - 2;

    const SinCosTables &tables = sin_cos_tables();
    const double index_cos = tables.cos_tab[index];
    const double index_sin = tables.sin_tab[index];

    return index_cos +
           delta * (-index_sin +
                    delta * (-index_cos * ONE_DIV_F2 + delta * (index_sin * ONE_DIV_F3 + delta * index_cos * ONE_DIV_F4)));
}

double SloppyMath::asin(double a) {
    bool negate_result = false;
    if (a < 0.0) {
        a = -a;
        negate_result = true;
    }

    if (a <= ASIN_MAX_VALUE_FOR_TABS) {
        const size_t index = static_cast<size_t>(a * ASIN_INDEXER + 0.5);
        const double delta = a - static_cast<double>(index) * ASIN_DELTA;
        const AsinTables &tables = asin_tables();
        const double result =
                tables.asin_tab[index] +
                delta * (tables.asin_der1_div_f1_tab[index] +
                         delta * (tables.asin_der2_div_f2_tab[index] +
                                  delta * (tables.asin_der3_div_f3_tab[index] +
                                           delta * tables.asin_der4_div_f4_tab[index])));
        return negate_result ? -result : result;
    }
    if (a < 1.0) {
        const double t = (1.0 - a) * 0.5;
        const double p =
                t * (ASIN_PS0 + t * (ASIN_PS1 + t * (ASIN_PS2 + t * (ASIN_PS3 + t * (ASIN_PS4 + t * ASIN_PS5)))));
        const double q = 1.0 + t * (ASIN_QS1 + t * (ASIN_QS2 + t * (ASIN_QS3 + t * ASIN_QS4)));
        const double s = std::sqrt(t);
        const double z = s + s * (p / q);
        const double result = ASIN_PIO2_HI - ((z + z) - ASIN_PIO2_LO);
        return negate_result ? -result : result;
    }
    if (a == 1.0) {
        return negate_result ? -std::numbers::pi / 2.0 : std::numbers::pi / 2.0;
    }
    return std::numeric_limits<double>::quiet_NaN();
}

} // namespace lucene
