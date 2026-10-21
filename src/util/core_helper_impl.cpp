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

module clucene:core_helper.impl;

import :core_helper;

import :types;
import std;

namespace clucene {

Result<size_t> CoreHelper::check_from_index_size(size_t from_index, size_t size, size_t length) {
    if (from_index > length || size > length - from_index) {
        return err(LuceneError::array_index_out_of_bounds(
            std::format("size: {} is too large, from_index: {}, length: {}", size, from_index, length)));
    }
    return from_index;
}

Result<size_t> CoreHelper::check_from_to_index(size_t from_index, size_t to_index, size_t length) {
    if (from_index > to_index || to_index > length) {
        return err(LuceneError::array_index_out_of_bounds(
            std::format("index out of bounds: from_index={} to_index={} length={}", from_index, to_index, length)));
    }
    return from_index;
}

Result<size_t> CoreHelper::check_index(size_t index, size_t length) {
    if (index >= length) {
        return err(LuceneError::array_index_out_of_bounds(
            std::format("index out of bounds: index={} length={}", index, length)));
    }
    return index;
}

int32_t CoreHelper::mismatch_u8(std::span<const uint8_t> a, std::span<const uint8_t> b) {
    const size_t common = std::min(a.size(), b.size());
    size_t i = 0;
    // TODO(clucene): rlucene uses 32-byte SIMD compares here; revisit once the
    // hot codec paths are ported and profiled.
    while (i < common && a[i] == b[i]) {
        ++i;
    }
    if (i < common || a.size() != b.size()) {
        return static_cast<int32_t>(i);
    }
    return -1;
}

int32_t CoreHelper::mismatch_i32(std::span<const int32_t> a, std::span<const int32_t> b) {
    const size_t common = std::min(a.size(), b.size());
    size_t i = 0;
    while (i < common && a[i] == b[i]) {
        ++i;
    }
    if (i < common || a.size() != b.size()) {
        return static_cast<int32_t>(i);
    }
    return -1;
}

int32_t CoreHelper::compare_f32(float a, float b) {
    const bool a_nan = std::isnan(a);
    const bool b_nan = std::isnan(b);
    if (a_nan || b_nan) {
        if (a_nan && b_nan) {
            return 0;
        }
        return a_nan ? 1 : -1;
    }
    if (a < b) {
        return -1;
    }
    if (a > b) {
        return 1;
    }
    // Same value: only the signed zeros still differ (-0.0 orders before +0.0).
    const bool a_negative = std::signbit(a);
    const bool b_negative = std::signbit(b);
    if (a_negative == b_negative) {
        return 0;
    }
    return a_negative ? -1 : 1;
}

int32_t CoreHelper::compare_f64(double a, double b) {
    const bool a_nan = std::isnan(a);
    const bool b_nan = std::isnan(b);
    if (a_nan || b_nan) {
        if (a_nan && b_nan) {
            return 0;
        }
        return a_nan ? 1 : -1;
    }
    if (a < b) {
        return -1;
    }
    if (a > b) {
        return 1;
    }
    const bool a_negative = std::signbit(a);
    const bool b_negative = std::signbit(b);
    if (a_negative == b_negative) {
        return 0;
    }
    return a_negative ? -1 : 1;
}

float CoreHelper::max_f32(float a, float b) {
    if (std::isnan(a)) {
        return a;
    }
    if (std::isnan(b)) {
        return b;
    }
    if (a == 0.0F && b == 0.0F) {
        return std::signbit(a) ? b : a;
    }
    return a >= b ? a : b;
}

float CoreHelper::min_f32(float a, float b) {
    if (std::isnan(a)) {
        return a;
    }
    if (std::isnan(b)) {
        return b;
    }
    if (a == 0.0F && b == 0.0F) {
        return std::signbit(a) ? a : b;
    }
    return a <= b ? a : b;
}

double CoreHelper::max_f64(double a, double b) {
    if (std::isnan(a)) {
        return a;
    }
    if (std::isnan(b)) {
        return b;
    }
    if (a == 0.0 && b == 0.0) {
        return std::signbit(a) ? b : a;
    }
    return a >= b ? a : b;
}

double CoreHelper::min_f64(double a, double b) {
    if (std::isnan(a)) {
        return a;
    }
    if (std::isnan(b)) {
        return b;
    }
    if (a == 0.0 && b == 0.0) {
        return std::signbit(a) ? a : b;
    }
    return a <= b ? a : b;
}

} // namespace clucene
