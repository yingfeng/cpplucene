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

module lucene_core:number.impl;

import :number;
import :types;

import std;

namespace lucene {

bool number_is_integral(const Number &number) {
    return number.index() <= 3;
}

double number_as_f64(const Number &value) {
    return std::visit([](auto v) { return static_cast<double>(v); }, value);
}

int64_t number_as_i64(const Number &value) {
    return std::visit([](auto v) { return static_cast<int64_t>(v); }, value);
}

int32_t number_as_i32(const Number &value) {
    return std::visit([](auto v) { return static_cast<int32_t>(v); }, value);
}

bool number_fits_i32(const Number &value) {
    return std::visit(
        [](auto v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_floating_point_v<T>) {
                return v >= static_cast<T>(std::numeric_limits<int32_t>::min()) &&
                       v <= static_cast<T>(std::numeric_limits<int32_t>::max());
            } else {
                return static_cast<int64_t>(v) >= std::numeric_limits<int32_t>::min() &&
                       static_cast<int64_t>(v) <= std::numeric_limits<int32_t>::max();
            }
        },
        value);
}

std::string number_to_string(const Number &value) {
    return std::visit(
        [](auto v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, uint8_t>) {
                return std::format("{}", static_cast<uint32_t>(v));
            } else if constexpr (std::is_floating_point_v<T>) {
                return std::format("{}", v);
            } else {
                return std::format("{}", v);
            }
        },
        value);
}

} // namespace lucene
