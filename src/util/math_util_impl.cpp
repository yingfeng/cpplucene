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

module clucene:math_util.impl;

import :math_util;

import :types;
import std;

namespace clucene {

Result<int32_t> MathUtil::log(int64_t x, int32_t base) {
    if (base == 2) {
        // This specialized method is significantly faster.
        if (x <= 0) {
            return 0;
        }
        return 63 - std::countl_zero(static_cast<uint64_t>(x));
    }
    if (base <= 1) {
        return err(LuceneError::illegal_argument("base must be > 1"));
    }

    int32_t ret = 0;
    while (x >= static_cast<int64_t>(base)) {
        x /= static_cast<int64_t>(base);
        ++ret;
    }
    return ret;
}

double MathUtil::log_f64(double base, double x) { return std::log(x) / std::log(base); }

int64_t MathUtil::gcd(int64_t a, int64_t b) {
    // `wrapping_abs` semantics: abs(INT64_MIN) stays INT64_MIN.
    const auto abs_i64 = [](int64_t v) -> uint64_t {
        return v < 0 ? static_cast<uint64_t>(0) - static_cast<uint64_t>(v) : static_cast<uint64_t>(v);
    };

    uint64_t ua = abs_i64(a);
    uint64_t ub = abs_i64(b);
    if (ua == 0) {
        return static_cast<int64_t>(ub);
    }
    if (ub == 0) {
        return static_cast<int64_t>(ua);
    }

    const uint32_t common_trailing_zeros = std::countr_zero(ua | ub);
    ua >>= std::countr_zero(ua);
    while (ub != 0) {
        ub >>= std::countr_zero(ub);
        if (ua == ub) {
            break;
        }
        if (ua > ub) {
            std::swap(ua, ub);
        }
        if (ua == 1) {
            break;
        }
        ub -= ua;
    }
    return static_cast<int64_t>(ua << common_trailing_zeros);
}

double MathUtil::asinh(double a) {
    // Check the sign bit of the raw representation to handle -0.
    const double sign = std::bit_cast<int64_t>(a) < 0 ? -1.0 : 1.0;
    return sign * std::log(std::sqrt(a * a + 1.0) + std::abs(a));
}

double MathUtil::acosh(double a) { return std::log(std::sqrt(a * a - 1.0) + a); }

double MathUtil::atanh(double a) {
    // Check the sign bit of the raw representation to handle -0.
    const double mult = std::bit_cast<int64_t>(a) < 0 ? -0.5 : 0.5;
    return mult * std::log((1.0 + std::abs(a)) / (1.0 - std::abs(a)));
}

double MathUtil::sum_relative_error_bound(int32_t num_values) {
    if (num_values <= 1) {
        return 0.0;
    }
    // Machine epsilon (unit roundoff), 2^-52.
    constexpr double u = std::bit_cast<double>(static_cast<uint64_t>(0x3CB0000000000000ull));
    return static_cast<double>(num_values - 1) * u;
}

double MathUtil::sum_upper_bound(double sum, int32_t num_values) {
    if (num_values <= 2) {
        return sum;
    }
    const double b = sum_relative_error_bound(num_values);
    return (1.0 + 2.0 * b) * sum;
}

} // namespace clucene
