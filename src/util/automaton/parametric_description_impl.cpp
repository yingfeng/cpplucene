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

module lucene_core:parametric_description.impl;

import :parametric_description;

import :debug;
import :types;

import std;

namespace lucene {

namespace {

/// Bit masks for `1..=63` bits, as used by [`unpack`].
constexpr std::array<int64_t, 63> MASKS{
        0x1LL,
        0x3LL,
        0x7LL,
        0xfLL,
        0x1fLL,
        0x3fLL,
        0x7fLL,
        0xffLL,
        0x1ffLL,
        0x3ffLL,
        0x7ffLL,
        0xfffLL,
        0x1fffLL,
        0x3fffLL,
        0x7fffLL,
        0xffffLL,
        0x1ffffLL,
        0x3ffffLL,
        0x7ffffLL,
        0xfffffLL,
        0x1fffffLL,
        0x3fffffLL,
        0x7fffffLL,
        0xffffffLL,
        0x1ffffffLL,
        0x3ffffffLL,
        0x7ffffffLL,
        0xfffffffLL,
        0x1fffffffLL,
        0x3fffffffLL,
        0x7fffffffLL,
        0xffffffffLL,
        0x1ffffffffLL,
        0x3ffffffffLL,
        0x7ffffffffLL,
        0xfffffffffLL,
        0x1fffffffffLL,
        0x3fffffffffLL,
        0x7fffffffffLL,
        0xffffffffffLL,
        0x1ffffffffffLL,
        0x3ffffffffffLL,
        0x7ffffffffffLL,
        0xfffffffffffLL,
        0x1fffffffffffLL,
        0x3fffffffffffLL,
        0x7fffffffffffLL,
        0xffffffffffffLL,
        0x1ffffffffffffLL,
        0x3ffffffffffffLL,
        0x7ffffffffffffLL,
        0xfffffffffffffLL,
        0x1fffffffffffffLL,
        0x3fffffffffffffLL,
        0x7fffffffffffffLL,
        0xffffffffffffffLL,
        0x1ffffffffffffffLL,
        0x3ffffffffffffffLL,
        0x7ffffffffffffffLL,
        0xfffffffffffffffLL,
        0x1fffffffffffffffLL,
        0x3fffffffffffffffLL,
        0x7fffffffffffffffLL,
};

} // namespace

int32_t unpack(std::span<const int64_t> data, int32_t index, size_t bits_per_value) {
    debug_check(bits_per_value >= 1 && bits_per_value <= 63, "the field width is in range");

    const auto bit_loc = static_cast<int64_t>(bits_per_value) * static_cast<int64_t>(index);
    const auto data_loc = static_cast<size_t>(bit_loc >> 6);
    const auto bit_start = static_cast<size_t>(bit_loc & 63);

    if (bit_start + bits_per_value <= 64) {
        return static_cast<int32_t>((data[data_loc] >> bit_start) & MASKS[bits_per_value - 1]);
    }

    const size_t part = 64 - bit_start;
    return static_cast<int32_t>(((data[data_loc] >> bit_start) & MASKS[part - 1]) +
                                ((data[1 + data_loc] & MASKS[bits_per_value - part - 1]) << part));
}

ParametricDescription::ParametricDescription(int32_t w, int32_t n, std::vector<int32_t> min_errors,
                                             std::shared_ptr<const ParametricDescriptionBase> sub)
    : w_(w), n_(n), min_errors_(std::move(min_errors)), sub_(std::move(sub)) {}

bool ParametricDescription::is_accept(int32_t abs_state) const {
    // Decode abs_state into state and offset.
    const int32_t state = abs_state / (w_ + 1);
    const int32_t offset = abs_state % (w_ + 1);
    debug_check(offset >= 0, "the offset is not negative");

    return w_ - offset + min_errors_[static_cast<size_t>(state)] <= n_;
}

int32_t ParametricDescription::transition(int32_t state, int32_t position, int32_t vector) const {
    return sub_->transition(state, position, vector, *this);
}

} // namespace lucene
