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

module lucene_core:posting_decoding_util.impl;

import :posting_decoding_util;

import :types;
import std;

import :error;
import :data_input;

namespace lucene {

namespace {

/// `value >> shift` with the 32-bit shift semantics of the Rust code: a shift
/// distance of 32 or more yields 0 instead of being undefined behaviour.
[[nodiscard]] inline uint32_t unsigned_shift(uint32_t value, int32_t shift) noexcept {
    return shift >= 32 ? 0u : (value >> static_cast<uint32_t>(shift));
}

} // namespace

VoidResult PostingDecodingUtil::split_ints_same(size_t count,
                                                std::span<int32_t> b_and_c,
                                                int32_t b_shift,
                                                int32_t dec,
                                                int32_t b_mask,
                                                size_t c_index,
                                                int32_t c_mask) {
    if (dec <= 0) {
        return err(LuceneError::illegal_argument("dec must be positive"));
    }
    auto read = input_.read_ints(b_and_c, c_index, count);
    if (!read) {
        return read;
    }

    const int32_t max_iter = (b_shift - 1) / dec;
    for (int32_t j = 0; j <= max_iter; ++j) {
        const int32_t shift = b_shift - j * dec;
        if (shift <= 0) {
            continue;
        }
        const size_t base = count * static_cast<size_t>(j);
        for (size_t i = 0; i < count; ++i) {
            b_and_c[base + i] = static_cast<int32_t>(unsigned_shift(static_cast<uint32_t>(b_and_c[c_index + i]), shift) &
                                                     static_cast<uint32_t>(b_mask));
        }
    }
    for (size_t i = 0; i < count; ++i) {
        b_and_c[c_index + i] &= c_mask;
    }
    return {};
}

VoidResult PostingDecodingUtil::split_ints_diff(size_t count,
                                                std::span<int32_t> b,
                                                int32_t b_shift,
                                                int32_t dec,
                                                int32_t b_mask,
                                                std::span<int32_t> c,
                                                size_t c_index,
                                                int32_t c_mask) {
    if (dec <= 0) {
        return err(LuceneError::illegal_argument("dec must be positive"));
    }
    auto read = input_.read_ints(c, c_index, count);
    if (!read) {
        return read;
    }

    const int32_t max_iter = (b_shift - 1) / dec;
    for (int32_t j = 0; j <= max_iter; ++j) {
        const int32_t shift = b_shift - j * dec;
        if (shift <= 0) {
            continue;
        }
        const size_t base = count * static_cast<size_t>(j);
        for (size_t i = 0; i < count; ++i) {
            b[base + i] = static_cast<int32_t>(unsigned_shift(static_cast<uint32_t>(c[c_index + i]), shift) &
                                               static_cast<uint32_t>(b_mask));
        }
    }
    for (size_t i = 0; i < count; ++i) {
        c[c_index + i] &= c_mask;
    }
    return {};
}

} // namespace lucene
