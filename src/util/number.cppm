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

export module lucene_core:number;

import :types;

import std;

namespace lucene {

/// A numeric field value.  Port of `core/util/number.rs`.
///
/// The reference implementation's `BigInt` variant is not ported: nothing in the codec layer needs
/// it, and it would drag in an arbitrary-precision dependency.
export using Number = std::variant<uint8_t, int16_t, int32_t, int64_t, float, double>;

/// `true` for the integral variants.
[[nodiscard]] bool number_is_integral(const Number &number);

/// The value widened to `double` (lossy for large `int64_t`).
[[nodiscard]] double number_as_f64(const Number &value);

/// The value widened to `int64_t`; floats are truncated toward zero.
[[nodiscard]] int64_t number_as_i64(const Number &value);

/// The value widened to `int32_t`.
[[nodiscard]] int32_t number_as_i32(const Number &value);

/// `true` when the value fits into an `int32_t` without loss.
[[nodiscard]] bool number_fits_i32(const Number &value);

/// Human readable rendering (`Display` in the reference implementation).
[[nodiscard]] std::string number_to_string(const Number &value);

} // namespace lucene
