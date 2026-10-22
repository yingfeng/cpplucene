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

module lucene_core:for_util.impl;

import :for_util;

import :types;
import std;

import :error;
import :debug;
import :data_output;
import :posting_decoding_util;

namespace lucene {

namespace {

/// `value << shift` with the Rust semantics: bits shifted out are discarded,
/// and a shift distance of 32 or more yields 0 (instead of undefined behaviour).
[[nodiscard]] inline uint32_t left_shift(uint32_t value, int32_t shift) noexcept {
    return shift >= 32 ? 0u : (shift <= 0 ? value : (value << static_cast<uint32_t>(shift)));
}

/// `value >> shift`; a shift distance of 32 or more yields 0.
[[nodiscard]] inline uint32_t right_shift(uint32_t value, int32_t shift) noexcept {
    return shift >= 32 ? 0u : (shift <= 0 ? value : (value >> static_cast<uint32_t>(shift)));
}

/// `ForUtil::MASK8_n`: `n` one-bits replicated over the four byte lanes.
[[nodiscard]] constexpr int32_t m8(int32_t bits) noexcept {
    return static_cast<int32_t>(0x01010101u * ((1u << static_cast<uint32_t>(bits)) - 1u));
}

/// `ForUtil::MASK16_n`: `n` one-bits replicated over the two short lanes.
[[nodiscard]] constexpr int32_t m16(int32_t bits) noexcept {
    return static_cast<int32_t>(0x00010001u * ((1u << static_cast<uint32_t>(bits)) - 1u));
}

// ---- lane (un)packing ----

void expand8_raw(std::span<int32_t> arr) {
    for (size_t i = 0; i < 32; ++i) {
        const uint32_t lanes = static_cast<uint32_t>(arr[i]);
        arr[i] = static_cast<int32_t>((lanes >> 24) & 0xFFu);
        arr[32 + i] = static_cast<int32_t>((lanes >> 16) & 0xFFu);
        arr[64 + i] = static_cast<int32_t>((lanes >> 8) & 0xFFu);
        arr[96 + i] = static_cast<int32_t>(lanes & 0xFFu);
    }
}

void collapse8_raw(std::span<int32_t> arr) {
    for (size_t i = 0; i < 32; ++i) {
        arr[i] = static_cast<int32_t>(left_shift(static_cast<uint32_t>(arr[i]), 24) |
                                     left_shift(static_cast<uint32_t>(arr[32 + i]), 16) |
                                     left_shift(static_cast<uint32_t>(arr[64 + i]), 8) |
                                     static_cast<uint32_t>(arr[96 + i]));
    }
}

void expand16_raw(std::span<int32_t> arr) {
    for (size_t i = 0; i < 64; ++i) {
        const uint32_t lanes = static_cast<uint32_t>(arr[i]);
        arr[i] = static_cast<int32_t>((lanes >> 16) & 0xFFFFu);
        arr[64 + i] = static_cast<int32_t>(lanes & 0xFFFFu);
    }
}

void collapse16_raw(std::span<int32_t> arr) {
    for (size_t i = 0; i < 64; ++i) {
        arr[i] = static_cast<int32_t>(left_shift(static_cast<uint32_t>(arr[i]), 16) |
                                     static_cast<uint32_t>(arr[64 + i]));
    }
}

} // namespace

// ---- specialised decoders (see `ForUtil::decode`) ----

VoidResult ForUtil::decode1(PostingDecodingUtil &pdu, std::span<int32_t> ints) {
    return pdu.split_ints_same(4, ints, 7, 1, m8(1), 28, m8(1));
}

VoidResult ForUtil::decode2(PostingDecodingUtil &pdu, std::span<int32_t> ints) {
    return pdu.split_ints_same(8, ints, 6, 2, m8(2), 24, m8(2));
}

VoidResult ForUtil::decode3(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints) {
    auto split = pdu.split_ints_diff(12, ints, 5, 3, m8(3), tmp, 0, m8(2));
    if (!split) {
        return split;
    }
    size_t tmp_idx = 0;
    size_t ints_idx = 24;
    for (int32_t iter = 0; iter < 4; ++iter) {
        uint32_t l0 = left_shift(static_cast<uint32_t>(tmp[tmp_idx]), 1);
        l0 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 1]), 1) & static_cast<uint32_t>(m8(1));
        ints[ints_idx] = static_cast<int32_t>(l0);

        uint32_t l1 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 1] & m8(1)), 2);
        l1 |= static_cast<uint32_t>(tmp[tmp_idx + 2]);
        ints[ints_idx + 1] = static_cast<int32_t>(l1);

        tmp_idx += 3;
        ints_idx += 2;
    }
    return {};
}

VoidResult ForUtil::decode4(PostingDecodingUtil &pdu, std::span<int32_t> ints) {
    return pdu.split_ints_same(16, ints, 4, 4, m8(4), 16, m8(4));
}

VoidResult ForUtil::decode5(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints) {
    auto split = pdu.split_ints_diff(20, ints, 3, 5, m8(5), tmp, 0, m8(3));
    if (!split) {
        return split;
    }
    size_t tmp_idx = 0;
    size_t ints_idx = 20;
    for (int32_t iter = 0; iter < 4; ++iter) {
        uint32_t l0 = left_shift(static_cast<uint32_t>(tmp[tmp_idx]), 2);
        l0 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 1]), 1) & static_cast<uint32_t>(m8(2));
        ints[ints_idx] = static_cast<int32_t>(l0);

        uint32_t l1 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 1] & m8(1)), 4);
        l1 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 2]), 1);
        l1 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 3]), 2) & static_cast<uint32_t>(m8(1));
        ints[ints_idx + 1] = static_cast<int32_t>(l1);

        uint32_t l2 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 3] & m8(2)), 3);
        l2 |= static_cast<uint32_t>(tmp[tmp_idx + 4]);
        ints[ints_idx + 2] = static_cast<int32_t>(l2);

        tmp_idx += 5;
        ints_idx += 3;
    }
    return {};
}

VoidResult ForUtil::decode6(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints) {
    auto split = pdu.split_ints_diff(24, ints, 2, 6, m8(6), tmp, 0, m8(2));
    if (!split) {
        return split;
    }
    for (size_t offset = 0; offset < 8; ++offset) {
        const size_t tmp_idx = offset * 3;
        uint32_t l0 = left_shift(static_cast<uint32_t>(tmp[tmp_idx]), 4);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 1]), 2);
        l0 |= static_cast<uint32_t>(tmp[tmp_idx + 2]);
        ints[24 + offset] = static_cast<int32_t>(l0);
    }
    return {};
}

VoidResult ForUtil::decode7(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints) {
    auto split = pdu.split_ints_diff(28, ints, 1, 7, m8(7), tmp, 0, m8(1));
    if (!split) {
        return split;
    }
    for (size_t offset = 0; offset < 4; ++offset) {
        const size_t tmp_idx = offset * 7;
        uint32_t l0 = left_shift(static_cast<uint32_t>(tmp[tmp_idx]), 6);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 1]), 5);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 2]), 4);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 3]), 3);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 4]), 2);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 5]), 1);
        l0 |= static_cast<uint32_t>(tmp[tmp_idx + 6]);
        ints[28 + offset] = static_cast<int32_t>(l0);
    }
    return {};
}

VoidResult ForUtil::decode8(PostingDecodingUtil &pdu, std::span<int32_t> ints) {
    return pdu.input().read_ints(ints, 0, 32);
}

VoidResult ForUtil::decode9(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints) {
    auto split = pdu.split_ints_diff(36, ints, 7, 9, m16(9), tmp, 0, m16(7));
    if (!split) {
        return split;
    }
    size_t tmp_idx = 0;
    size_t ints_idx = 36;
    for (int32_t iter = 0; iter < 4; ++iter) {
        uint32_t l0 = left_shift(static_cast<uint32_t>(tmp[tmp_idx]), 2);
        l0 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 1]), 5) & static_cast<uint32_t>(m16(2));
        ints[ints_idx] = static_cast<int32_t>(l0);

        uint32_t l1 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 1] & m16(5)), 4);
        l1 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 2]), 3) & static_cast<uint32_t>(m16(4));
        ints[ints_idx + 1] = static_cast<int32_t>(l1);

        uint32_t l2 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 2] & m16(3)), 6);
        l2 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 3]), 1) & static_cast<uint32_t>(m16(6));
        ints[ints_idx + 2] = static_cast<int32_t>(l2);

        uint32_t l3 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 3] & m16(1)), 8);
        l3 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 4]), 1);
        l3 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 5]), 6) & static_cast<uint32_t>(m16(1));
        ints[ints_idx + 3] = static_cast<int32_t>(l3);

        uint32_t l4 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 5] & m16(6)), 3);
        l4 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 6]), 4) & static_cast<uint32_t>(m16(3));
        ints[ints_idx + 4] = static_cast<int32_t>(l4);

        uint32_t l5 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 6] & m16(4)), 5);
        l5 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 7]), 2) & static_cast<uint32_t>(m16(5));
        ints[ints_idx + 5] = static_cast<int32_t>(l5);

        uint32_t l6 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 7] & m16(2)), 7);
        l6 |= static_cast<uint32_t>(tmp[tmp_idx + 8]);
        ints[ints_idx + 6] = static_cast<int32_t>(l6);

        tmp_idx += 9;
        ints_idx += 7;
    }
    return {};
}

VoidResult ForUtil::decode10(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints) {
    auto split = pdu.split_ints_diff(40, ints, 6, 10, m16(10), tmp, 0, m16(6));
    if (!split) {
        return split;
    }
    size_t tmp_idx = 0;
    size_t ints_idx = 40;
    for (int32_t iter = 0; iter < 8; ++iter) {
        uint32_t l0 = left_shift(static_cast<uint32_t>(tmp[tmp_idx]), 4);
        l0 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 1]), 2) & static_cast<uint32_t>(m16(4));
        ints[ints_idx] = static_cast<int32_t>(l0);

        uint32_t l1 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 1] & m16(2)), 8);
        l1 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 2]), 2);
        l1 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 3]), 4) & static_cast<uint32_t>(m16(2));
        ints[ints_idx + 1] = static_cast<int32_t>(l1);

        uint32_t l2 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 3] & m16(4)), 6);
        l2 |= static_cast<uint32_t>(tmp[tmp_idx + 4]);
        ints[ints_idx + 2] = static_cast<int32_t>(l2);

        tmp_idx += 5;
        ints_idx += 3;
    }
    return {};
}

VoidResult ForUtil::decode11(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints) {
    auto split = pdu.split_ints_diff(44, ints, 5, 11, m16(11), tmp, 0, m16(5));
    if (!split) {
        return split;
    }
    size_t tmp_idx = 0;
    size_t ints_idx = 44;
    for (int32_t iter = 0; iter < 4; ++iter) {
        uint32_t l0 = left_shift(static_cast<uint32_t>(tmp[tmp_idx]), 6);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 1]), 1);
        l0 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 2]), 4) & static_cast<uint32_t>(m16(1));
        ints[ints_idx] = static_cast<int32_t>(l0);

        uint32_t l1 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 2] & m16(4)), 7);
        l1 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 3]), 2);
        l1 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 4]), 3) & static_cast<uint32_t>(m16(2));
        ints[ints_idx + 1] = static_cast<int32_t>(l1);

        uint32_t l2 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 4] & m16(3)), 8);
        l2 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 5]), 3);
        l2 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 6]), 2) & static_cast<uint32_t>(m16(3));
        ints[ints_idx + 2] = static_cast<int32_t>(l2);

        uint32_t l3 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 6] & m16(2)), 9);
        l3 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 7]), 4);
        l3 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 8]), 1) & static_cast<uint32_t>(m16(4));
        ints[ints_idx + 3] = static_cast<int32_t>(l3);

        uint32_t l4 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 8] & m16(1)), 10);
        l4 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 9]), 5);
        l4 |= static_cast<uint32_t>(tmp[tmp_idx + 10]);
        ints[ints_idx + 4] = static_cast<int32_t>(l4);

        tmp_idx += 11;
        ints_idx += 5;
    }
    return {};
}

VoidResult ForUtil::decode12(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints) {
    auto split = pdu.split_ints_diff(48, ints, 4, 12, m16(12), tmp, 0, m16(4));
    if (!split) {
        return split;
    }
    for (size_t offset = 0; offset < 16; ++offset) {
        const size_t tmp_idx = offset * 3;
        const uint32_t l0 = left_shift(static_cast<uint32_t>(tmp[tmp_idx]), 8) |
                            left_shift(static_cast<uint32_t>(tmp[tmp_idx + 1]), 4) |
                            static_cast<uint32_t>(tmp[tmp_idx + 2]);
        ints[48 + offset] = static_cast<int32_t>(l0);
    }
    return {};
}

VoidResult ForUtil::decode13(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints) {
    auto split = pdu.split_ints_diff(52, ints, 3, 13, m16(13), tmp, 0, m16(3));
    if (!split) {
        return split;
    }
    size_t tmp_idx = 0;
    size_t ints_idx = 52;
    for (int32_t iter = 0; iter < 4; ++iter) {
        uint32_t l0 = left_shift(static_cast<uint32_t>(tmp[tmp_idx]), 10);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 1]), 7);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 2]), 4);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 3]), 1);
        l0 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 4]), 2) & static_cast<uint32_t>(m16(1));
        ints[ints_idx] = static_cast<int32_t>(l0);

        uint32_t l1 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 4] & m16(2)), 11);
        l1 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 5]), 8);
        l1 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 6]), 5);
        l1 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 7]), 2);
        l1 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 8]), 1) & static_cast<uint32_t>(m16(2));
        ints[ints_idx + 1] = static_cast<int32_t>(l1);

        uint32_t l2 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 8] & m16(1)), 12);
        l2 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 9]), 9);
        l2 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 10]), 6);
        l2 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 11]), 3);
        l2 |= static_cast<uint32_t>(tmp[tmp_idx + 12]);
        ints[ints_idx + 2] = static_cast<int32_t>(l2);

        tmp_idx += 13;
        ints_idx += 3;
    }
    return {};
}

VoidResult ForUtil::decode14(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints) {
    auto split = pdu.split_ints_diff(56, ints, 2, 14, m16(14), tmp, 0, m16(2));
    if (!split) {
        return split;
    }
    for (size_t offset = 0; offset < 8; ++offset) {
        const size_t tmp_idx = offset * 7;
        uint32_t l0 = left_shift(static_cast<uint32_t>(tmp[tmp_idx]), 12);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 1]), 10);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 2]), 8);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 3]), 6);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 4]), 4);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 5]), 2);
        l0 |= static_cast<uint32_t>(tmp[tmp_idx + 6]);
        ints[56 + offset] = static_cast<int32_t>(l0);
    }
    return {};
}

VoidResult ForUtil::decode15(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints) {
    auto split = pdu.split_ints_diff(60, ints, 1, 15, m16(15), tmp, 0, m16(1));
    if (!split) {
        return split;
    }
    for (size_t offset = 0; offset < 4; ++offset) {
        const size_t tmp_idx = offset * 15;
        uint32_t l0 = left_shift(static_cast<uint32_t>(tmp[tmp_idx]), 14);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 1]), 13);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 2]), 12);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 3]), 11);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 4]), 10);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 5]), 9);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 6]), 8);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 7]), 7);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 8]), 6);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 9]), 5);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 10]), 4);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 11]), 3);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 12]), 2);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 13]), 1);
        l0 |= static_cast<uint32_t>(tmp[tmp_idx + 14]);
        ints[60 + offset] = static_cast<int32_t>(l0);
    }
    return {};
}

VoidResult ForUtil::decode16(PostingDecodingUtil &pdu, std::span<int32_t> ints) {
    return pdu.input().read_ints(ints, 0, 64);
}

void ForUtil::expand8(std::span<int32_t> arr) {
    debug_check(arr.size() >= BLOCK_SIZE, "expand8 requires 128 values");
    expand8_raw(arr);
}

void ForUtil::collapse8(std::span<int32_t> arr) {
    debug_check(arr.size() >= BLOCK_SIZE, "collapse8 requires 128 values");
    collapse8_raw(arr);
}

void ForUtil::expand16(std::span<int32_t> arr) {
    debug_check(arr.size() >= BLOCK_SIZE, "expand16 requires 128 values");
    expand16_raw(arr);
}

void ForUtil::collapse16(std::span<int32_t> arr) {
    debug_check(arr.size() >= BLOCK_SIZE, "collapse16 requires 128 values");
    collapse16_raw(arr);
}

VoidResult ForUtil::encode(std::span<int32_t> ints, int32_t bits_per_value, DataOutput &out) {
    debug_check(ints.size() >= BLOCK_SIZE, "encode requires 128 values");
    int32_t primitive_size = 32;
    if (bits_per_value <= 8) {
        collapse8(ints);
        primitive_size = 8;
    } else if (bits_per_value <= 16) {
        collapse16(ints);
        primitive_size = 16;
    }
    return encode_with_tmp(ints, bits_per_value, primitive_size, out, tmp_);
}

VoidResult ForUtil::encode_with_tmp(std::span<const int32_t> ints,
                                    int32_t bits_per_value,
                                    int32_t primitive_size,
                                    DataOutput &out,
                                    std::span<int32_t> tmp) {
    if (bits_per_value < 1 || bits_per_value > 32) {
        return err(LuceneError::illegal_argument("bits_per_value must be in [1, 32]"));
    }
    const size_t num_ints = BLOCK_SIZE * static_cast<size_t>(primitive_size) / 32;
    const size_t num_ints_per_shift = static_cast<size_t>(bits_per_value) * 4;
    if (ints.size() < num_ints) {
        return err(LuceneError::illegal_argument("ints must hold num_ints values"));
    }
    if (tmp.size() < num_ints_per_shift) {
        return err(LuceneError::illegal_argument("tmp must hold bits_per_value * 4 values"));
    }

    // Fill the first plane, then OR the following planes into it until the
    // primitive no longer has room for a full value.
    size_t idx = 0;
    int32_t shift = primitive_size - bits_per_value;
    for (size_t t = 0; t < num_ints_per_shift; ++t) {
        tmp[t] = static_cast<int32_t>(left_shift(static_cast<uint32_t>(ints[idx + t]), shift));
    }
    idx += num_ints_per_shift;

    shift -= bits_per_value;
    while (shift >= 0) {
        for (size_t t = 0; t < num_ints_per_shift; ++t) {
            tmp[t] |= static_cast<int32_t>(left_shift(static_cast<uint32_t>(ints[idx + t]), shift));
        }
        idx += num_ints_per_shift;
        shift -= bits_per_value;
    }

    // The last plane is partially filled, so values straddle plane boundaries.
    const int32_t remaining_bits_per_int = shift + bits_per_value;
    const size_t remaining_bits_per_int_index = static_cast<size_t>(remaining_bits_per_int);
    const int32_t mask_remaining_bits_per_int = mask_for(primitive_size, remaining_bits_per_int_index);

    size_t tmp_idx = 0;
    int32_t remaining_bits_per_value = bits_per_value;
    while (idx < num_ints) {
        if (remaining_bits_per_value >= remaining_bits_per_int) {
            remaining_bits_per_value -= remaining_bits_per_int;
            tmp[tmp_idx] |= static_cast<int32_t>(
                    right_shift(static_cast<uint32_t>(ints[idx]), remaining_bits_per_value) &
                    static_cast<uint32_t>(mask_remaining_bits_per_int));
            if (remaining_bits_per_value == 0) {
                ++idx;
                remaining_bits_per_value = bits_per_value;
            }
            ++tmp_idx;
        } else {
            const size_t remaining_bits_per_value_index = static_cast<size_t>(remaining_bits_per_value);
            const int32_t mask1 = mask_for(primitive_size, remaining_bits_per_value_index);
            const int32_t mask2 =
                    mask_for(primitive_size, remaining_bits_per_int_index - remaining_bits_per_value_index);

            tmp[tmp_idx] |= static_cast<int32_t>(
                    left_shift(static_cast<uint32_t>(ints[idx] & mask1),
                               remaining_bits_per_int - remaining_bits_per_value));
            ++idx;
            remaining_bits_per_value += bits_per_value - remaining_bits_per_int;
            tmp[tmp_idx] |= static_cast<int32_t>(
                    right_shift(static_cast<uint32_t>(ints[idx]), remaining_bits_per_value) &
                    static_cast<uint32_t>(mask2));
            ++tmp_idx;
        }
    }

    for (size_t t = 0; t < num_ints_per_shift; ++t) {
        auto written = out.write_int(tmp[t]);
        if (!written) {
            return written;
        }
    }
    return {};
}

VoidResult ForUtil::decode_slow(int32_t bits_per_value,
                                PostingDecodingUtil &pdu,
                                std::span<int32_t> tmp,
                                std::span<int32_t> ints) {
    // rlucene only reaches this method for `bits_per_value >= 17` (the
    // specialised decoders below cover 1..16); the bit cursor starts negative
    // for smaller widths.
    if (bits_per_value < 17 || bits_per_value > 32) {
        return err(LuceneError::illegal_argument("decode_slow requires bits_per_value in [17, 32]"));
    }
    const size_t num_ints = static_cast<size_t>(bits_per_value) * 4;
    if (tmp.size() < num_ints) {
        return err(LuceneError::illegal_argument("tmp must hold bits_per_value * 4 values"));
    }
    if (ints.size() < num_ints) {
        return err(LuceneError::illegal_argument("ints must hold bits_per_value * 4 values"));
    }

    if (bits_per_value == 32) {
        // Every value occupies a whole int: the "packed" layout is the input.
        return pdu.input().read_ints(ints, 0, std::min(BLOCK_SIZE, ints.size()));
    }

    const int32_t mask = mask32(bits_per_value);
    auto split = pdu.split_ints_diff(num_ints, ints, 32 - bits_per_value, 32, mask, tmp, 0, -1);
    if (!split) {
        return split;
    }

    const size_t remaining_bits_per_int = static_cast<size_t>(32 - bits_per_value);
    const int32_t mask32_remaining_bits_per_int = mask32(static_cast<int32_t>(remaining_bits_per_int));

    size_t tmp_idx = 0;
    size_t remaining_bits = remaining_bits_per_int;
    const size_t out_end = std::min(BLOCK_SIZE, ints.size());
    for (size_t out_idx = num_ints; out_idx < out_end; ++out_idx) {
        int32_t b = bits_per_value - static_cast<int32_t>(remaining_bits);
        uint32_t value = left_shift(static_cast<uint32_t>(tmp[tmp_idx] & mask32(static_cast<int32_t>(remaining_bits))), b);
        ++tmp_idx;

        while (b >= static_cast<int32_t>(remaining_bits_per_int)) {
            b -= static_cast<int32_t>(remaining_bits_per_int);
            value |= left_shift(static_cast<uint32_t>(tmp[tmp_idx] & mask32_remaining_bits_per_int), b);
            ++tmp_idx;
        }

        if (b > 0) {
            value |= right_shift(static_cast<uint32_t>(tmp[tmp_idx]),
                                 static_cast<int32_t>(remaining_bits_per_int) - b) &
                     static_cast<uint32_t>(mask32(b));
            remaining_bits = remaining_bits_per_int - static_cast<size_t>(b);
        } else {
            remaining_bits = remaining_bits_per_int;
        }
        ints[out_idx] = static_cast<int32_t>(value);
    }
    return {};
}

VoidResult ForUtil::decode(int32_t bits_per_value, PostingDecodingUtil &pdu, std::span<int32_t> ints) {
    if (ints.size() < BLOCK_SIZE) {
        return err(LuceneError::illegal_argument("decode requires 128 values"));
    }

    VoidResult decoded;
    switch (bits_per_value) {
        case 1:
            decoded = decode1(pdu, ints);
            break;
        case 2:
            decoded = decode2(pdu, ints);
            break;
        case 3:
            decoded = decode3(pdu, tmp_, ints);
            break;
        case 4:
            decoded = decode4(pdu, ints);
            break;
        case 5:
            decoded = decode5(pdu, tmp_, ints);
            break;
        case 6:
            decoded = decode6(pdu, tmp_, ints);
            break;
        case 7:
            decoded = decode7(pdu, tmp_, ints);
            break;
        case 8:
            decoded = decode8(pdu, ints);
            break;
        case 9:
            decoded = decode9(pdu, tmp_, ints);
            break;
        case 10:
            decoded = decode10(pdu, tmp_, ints);
            break;
        case 11:
            decoded = decode11(pdu, tmp_, ints);
            break;
        case 12:
            decoded = decode12(pdu, tmp_, ints);
            break;
        case 13:
            decoded = decode13(pdu, tmp_, ints);
            break;
        case 14:
            decoded = decode14(pdu, tmp_, ints);
            break;
        case 15:
            decoded = decode15(pdu, tmp_, ints);
            break;
        case 16:
            decoded = decode16(pdu, ints);
            break;
        default:
            decoded = decode_slow(bits_per_value, pdu, tmp_, ints);
            break;
    }
    if (!decoded) {
        return decoded;
    }

    if (bits_per_value <= 8) {
        expand8(ints);
    } else if (bits_per_value <= 16) {
        expand16(ints);
    }
    return {};
}

} // namespace lucene
