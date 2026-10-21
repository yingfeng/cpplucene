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

export module clucene:math_util;

import :types;
import std;

import :error;

namespace clucene {

/// Math utility methods.  Port of `core/util/math_util.rs`.
export struct MathUtil {
    /// Returns `x <= 0 ? 0 : floor(log(x) / log(base))`.
    ///
    /// Returns `illegal_argument` when `base <= 1`.
    [[nodiscard]] static Result<int32_t> log(int64_t x, int32_t base);

    /// Calculates logarithm in a given base with floating-point numbers.
    [[nodiscard]] static double log_f64(double base, double x);

    /// Greatest common divisor of `a` and `b`.
    ///
    /// A GCD must be positive, but `2^64` cannot be expressed as an `int64_t`,
    /// although it is the GCD of `INT64_MIN` and `0`, as well as `INT64_MIN`
    /// and `INT64_MIN`. In these two cases this method returns `INT64_MIN`.
    [[nodiscard]] static int64_t gcd(int64_t a, int64_t b);

    /// Inverse hyperbolic sine, handling `-0`.
    [[nodiscard]] static double asinh(double a);
    /// Inverse hyperbolic cosine.
    [[nodiscard]] static double acosh(double a);
    /// Inverse hyperbolic tangent, handling `-0`.
    [[nodiscard]] static double atanh(double a);

    /// Relative error bound for the sum of `num_values` positive doubles
    /// computed with recursive summation (Higham 1993, formula 3.5).
    [[nodiscard]] static double sum_relative_error_bound(int32_t num_values);

    /// Maximum possible sum across `num_values` non-negative doubles, assuming
    /// one sum yielded `sum`.
    [[nodiscard]] static double sum_upper_bound(double sum, int32_t num_values);
};

} // namespace clucene
