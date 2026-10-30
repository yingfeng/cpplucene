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

module lucene_core:for_delta_util.impl;

import :for_delta_util;

import :types;
import std;

import :error;
import :debug;
import :data_output;
import :for_util;
import :packed_ints;
import :pfor_util;
import :posting_decoding_util;

namespace lucene {

namespace {

[[nodiscard]] inline uint32_t left_shift(uint32_t value, int32_t shift) noexcept {
    return shift >= 32 ? 0u : (shift <= 0 ? value : (value << static_cast<uint32_t>(shift)));
}

[[nodiscard]] inline uint32_t right_shift(uint32_t value, int32_t shift) noexcept {
    return shift >= 32 ? 0u : (shift <= 0 ? value : (value >> static_cast<uint32_t>(shift)));
}

/// `a + b` on the `int32_t` wraparound semantics of Rust's `wrapping_add`.
[[nodiscard]] inline int32_t wrapping_add(int32_t a, int32_t b) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}

/// Prefix sum of `arr[0..n)`; the lane trick below relies on the sums being
/// small enough not to carry between lanes, which the caller guarantees.
void prefix_sum_small(std::span<int32_t> arr, size_t n) {
    uint32_t carry = 0;
    for (size_t i = 0; i < n; ++i) {
        carry += static_cast<uint32_t>(arr[i]);
        arr[i] = static_cast<int32_t>(carry);
    }
}

// ---- decoders that produce the 16-bit lane layout (delta variant) ----

VoidResult decode_4_to_16(PostingDecodingUtil &pdu, std::span<int32_t> ints) {
    return pdu.split_ints_same(16, ints, 12, 4, ForUtil::mask16(4), 48, ForUtil::mask16(4));
}

VoidResult decode_5_to_16(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints) {
    auto split = pdu.split_ints_diff(20, ints, 11, 5, ForUtil::mask16(5), tmp, 0, ForUtil::mask16(1));
    if (!split) {
        return split;
    }
    for (size_t offset = 0; offset < 4; ++offset) {
        const size_t tmp_idx = offset * 5;
        uint32_t l0 = left_shift(static_cast<uint32_t>(tmp[tmp_idx]), 4);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 1]), 3);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 2]), 2);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 3]), 1);
        l0 |= static_cast<uint32_t>(tmp[tmp_idx + 4]);
        ints[60 + offset] = static_cast<int32_t>(l0);
    }
    return {};
}

VoidResult decode_6_to_16(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints) {
    auto split = pdu.split_ints_diff(24, ints, 10, 6, ForUtil::mask16(6), tmp, 0, ForUtil::mask16(4));
    if (!split) {
        return split;
    }
    size_t tmp_idx = 0;
    size_t ints_idx = 48;
    for (size_t iter = 0; iter < 8; ++iter) {
        uint32_t l0 = left_shift(static_cast<uint32_t>(tmp[tmp_idx]), 2);
        l0 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 1]), 2) &
              static_cast<uint32_t>(ForUtil::mask16(2));
        ints[ints_idx] = static_cast<int32_t>(l0);

        uint32_t l1 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 1] & ForUtil::mask16(2)), 4);
        l1 |= static_cast<uint32_t>(tmp[tmp_idx + 2]);
        ints[ints_idx + 1] = static_cast<int32_t>(l1);

        tmp_idx += 3;
        ints_idx += 2;
    }
    return {};
}

VoidResult decode_7_to_16(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints) {
    auto split = pdu.split_ints_diff(28, ints, 9, 7, ForUtil::mask16(7), tmp, 0, ForUtil::mask16(2));
    if (!split) {
        return split;
    }
    size_t tmp_idx = 0;
    size_t ints_idx = 56;
    for (size_t iter = 0; iter < 4; ++iter) {
        uint32_t l0 = left_shift(static_cast<uint32_t>(tmp[tmp_idx]), 5);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 1]), 3);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 2]), 1);
        l0 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 3]), 1) &
              static_cast<uint32_t>(ForUtil::mask16(1));
        ints[ints_idx] = static_cast<int32_t>(l0);

        uint32_t l1 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 3] & ForUtil::mask16(1)), 6);
        l1 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 4]), 4);
        l1 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 5]), 2);
        l1 |= static_cast<uint32_t>(tmp[tmp_idx + 6]);
        ints[ints_idx + 1] = static_cast<int32_t>(l1);

        tmp_idx += 7;
        ints_idx += 2;
    }
    return {};
}

VoidResult decode_8_to_16(PostingDecodingUtil &pdu, std::span<int32_t> ints) {
    return pdu.split_ints_same(32, ints, 8, 8, ForUtil::mask16(8), 32, ForUtil::mask16(8));
}

// ---- decoders that produce the 32-bit layout (delta variant) ----

VoidResult decode_11_to_32(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints) {
    auto split = pdu.split_ints_diff(44, ints, 21, 11, ForUtil::mask32(11), tmp, 0, ForUtil::mask32(10));
    if (!split) {
        return split;
    }
    size_t tmp_idx = 0;
    size_t ints_idx = 88;
    for (size_t iter = 0; iter < 4; ++iter) {
        uint32_t l0 = left_shift(static_cast<uint32_t>(tmp[tmp_idx]), 1);
        l0 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 1]), 9) &
              static_cast<uint32_t>(ForUtil::mask32(1));
        ints[ints_idx] = static_cast<int32_t>(l0);

        uint32_t l1 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 1] & ForUtil::mask32(9)), 2);
        l1 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 2]), 8) &
              static_cast<uint32_t>(ForUtil::mask32(2));
        ints[ints_idx + 1] = static_cast<int32_t>(l1);

        uint32_t l2 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 2] & ForUtil::mask32(8)), 3);
        l2 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 3]), 7) &
              static_cast<uint32_t>(ForUtil::mask32(3));
        ints[ints_idx + 2] = static_cast<int32_t>(l2);

        uint32_t l3 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 3] & ForUtil::mask32(7)), 4);
        l3 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 4]), 6) &
              static_cast<uint32_t>(ForUtil::mask32(4));
        ints[ints_idx + 3] = static_cast<int32_t>(l3);

        uint32_t l4 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 4] & ForUtil::mask32(6)), 5);
        l4 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 5]), 5) &
              static_cast<uint32_t>(ForUtil::mask32(5));
        ints[ints_idx + 4] = static_cast<int32_t>(l4);

        uint32_t l5 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 5] & ForUtil::mask32(5)), 6);
        l5 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 6]), 4) &
              static_cast<uint32_t>(ForUtil::mask32(6));
        ints[ints_idx + 5] = static_cast<int32_t>(l5);

        uint32_t l6 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 6] & ForUtil::mask32(4)), 7);
        l6 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 7]), 3) &
              static_cast<uint32_t>(ForUtil::mask32(7));
        ints[ints_idx + 6] = static_cast<int32_t>(l6);

        uint32_t l7 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 7] & ForUtil::mask32(3)), 8);
        l7 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 8]), 2) &
              static_cast<uint32_t>(ForUtil::mask32(8));
        ints[ints_idx + 7] = static_cast<int32_t>(l7);

        uint32_t l8 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 8] & ForUtil::mask32(2)), 9);
        l8 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 9]), 1) &
              static_cast<uint32_t>(ForUtil::mask32(9));
        ints[ints_idx + 8] = static_cast<int32_t>(l8);

        uint32_t l9 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 9] & ForUtil::mask32(1)), 10);
        l9 |= static_cast<uint32_t>(tmp[tmp_idx + 10]);
        ints[ints_idx + 9] = static_cast<int32_t>(l9);

        tmp_idx += 11;
        ints_idx += 10;
    }
    return {};
}

VoidResult decode_12_to_32(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints) {
    auto split = pdu.split_ints_diff(48, ints, 20, 12, ForUtil::mask32(12), tmp, 0, ForUtil::mask32(8));
    if (!split) {
        return split;
    }
    size_t tmp_idx = 0;
    size_t ints_idx = 96;
    for (size_t iter = 0; iter < 16; ++iter) {
        uint32_t l0 = left_shift(static_cast<uint32_t>(tmp[tmp_idx]), 4);
        l0 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 1]), 4) &
              static_cast<uint32_t>(ForUtil::mask32(4));
        ints[ints_idx] = static_cast<int32_t>(l0);

        uint32_t l1 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 1] & ForUtil::mask32(4)), 8);
        l1 |= static_cast<uint32_t>(tmp[tmp_idx + 2]);
        ints[ints_idx + 1] = static_cast<int32_t>(l1);

        tmp_idx += 3;
        ints_idx += 2;
    }
    return {};
}

VoidResult decode_13_to_32(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints) {
    auto split = pdu.split_ints_diff(52, ints, 19, 13, ForUtil::mask32(13), tmp, 0, ForUtil::mask32(6));
    if (!split) {
        return split;
    }
    size_t tmp_idx = 0;
    size_t ints_idx = 104;
    for (size_t iter = 0; iter < 4; ++iter) {
        uint32_t l0 = left_shift(static_cast<uint32_t>(tmp[tmp_idx]), 7);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 1]), 1);
        l0 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 2]), 5) &
              static_cast<uint32_t>(ForUtil::mask32(1));
        ints[ints_idx] = static_cast<int32_t>(l0);

        uint32_t l1 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 2] & ForUtil::mask32(5)), 8);
        l1 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 3]), 2);
        l1 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 4]), 4) &
              static_cast<uint32_t>(ForUtil::mask32(2));
        ints[ints_idx + 1] = static_cast<int32_t>(l1);

        uint32_t l2 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 4] & ForUtil::mask32(4)), 9);
        l2 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 5]), 3);
        l2 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 6]), 3) &
              static_cast<uint32_t>(ForUtil::mask32(3));
        ints[ints_idx + 2] = static_cast<int32_t>(l2);

        uint32_t l3 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 6] & ForUtil::mask32(3)), 10);
        l3 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 7]), 4);
        l3 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 8]), 2) &
              static_cast<uint32_t>(ForUtil::mask32(4));
        ints[ints_idx + 3] = static_cast<int32_t>(l3);

        uint32_t l4 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 8] & ForUtil::mask32(2)), 11);
        l4 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 9]), 5);
        l4 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 10]), 1) &
              static_cast<uint32_t>(ForUtil::mask32(5));
        ints[ints_idx + 4] = static_cast<int32_t>(l4);

        uint32_t l5 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 10] & ForUtil::mask32(1)), 12);
        l5 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 11]), 6);
        l5 |= static_cast<uint32_t>(tmp[tmp_idx + 12]);
        ints[ints_idx + 5] = static_cast<int32_t>(l5);

        tmp_idx += 13;
        ints_idx += 6;
    }
    return {};
}

VoidResult decode_14_to_32(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints) {
    auto split = pdu.split_ints_diff(56, ints, 18, 14, ForUtil::mask32(14), tmp, 0, ForUtil::mask32(4));
    if (!split) {
        return split;
    }
    size_t tmp_idx = 0;
    size_t ints_idx = 112;
    for (size_t iter = 0; iter < 8; ++iter) {
        uint32_t l0 = left_shift(static_cast<uint32_t>(tmp[tmp_idx]), 10);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 1]), 6);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 2]), 2);
        l0 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 3]), 2) &
              static_cast<uint32_t>(ForUtil::mask32(2));
        ints[ints_idx] = static_cast<int32_t>(l0);

        uint32_t l1 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 3] & ForUtil::mask32(2)), 12);
        l1 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 4]), 8);
        l1 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 5]), 4);
        l1 |= static_cast<uint32_t>(tmp[tmp_idx + 6]);
        ints[ints_idx + 1] = static_cast<int32_t>(l1);

        tmp_idx += 7;
        ints_idx += 2;
    }
    return {};
}

VoidResult decode_15_to_32(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints) {
    auto split = pdu.split_ints_diff(60, ints, 17, 15, ForUtil::mask32(15), tmp, 0, ForUtil::mask32(2));
    if (!split) {
        return split;
    }
    size_t tmp_idx = 0;
    size_t ints_idx = 120;
    for (size_t iter = 0; iter < 4; ++iter) {
        uint32_t l0 = left_shift(static_cast<uint32_t>(tmp[tmp_idx]), 13);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 1]), 11);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 2]), 9);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 3]), 7);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 4]), 5);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 5]), 3);
        l0 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 6]), 1);
        l0 |= right_shift(static_cast<uint32_t>(tmp[tmp_idx + 7]), 1) &
              static_cast<uint32_t>(ForUtil::mask32(1));
        ints[ints_idx] = static_cast<int32_t>(l0);

        uint32_t l1 = left_shift(static_cast<uint32_t>(tmp[tmp_idx + 7] & ForUtil::mask32(1)), 14);
        l1 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 8]), 12);
        l1 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 9]), 10);
        l1 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 10]), 8);
        l1 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 11]), 6);
        l1 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 12]), 4);
        l1 |= left_shift(static_cast<uint32_t>(tmp[tmp_idx + 13]), 2);
        l1 |= static_cast<uint32_t>(tmp[tmp_idx + 14]);
        ints[ints_idx + 1] = static_cast<int32_t>(l1);

        tmp_idx += 15;
        ints_idx += 2;
    }
    return {};
}

VoidResult decode_16_to_32(PostingDecodingUtil &pdu, std::span<int32_t> ints) {
    return pdu.split_ints_same(64, ints, 16, 16, ForUtil::mask32(16), 64, ForUtil::mask32(16));
}

} // namespace

void ForDeltaUtil::prefix_sum_of_ones(std::span<int32_t> arr, int32_t base) {
    debug_check(arr.size() >= BLOCK_SIZE, "prefix_sum_of_ones requires 128 values");
    for (size_t i = 0; i < BLOCK_SIZE; ++i) {
        // base + (i + 1)
        arr[i] = wrapping_add(base, static_cast<int32_t>(i + 1));
    }
}

void ForDeltaUtil::prefix_sum8(std::span<int32_t> arr, int32_t base) {
    debug_check(arr.size() >= BLOCK_SIZE, "prefix_sum8 requires 128 values");
    // Values use at most 3 bits, so summing 32 of them in the 8-bit lanes
    // cannot carry into the neighbour lane.
    prefix_sum_small(arr.first(32), 32);

    const uint32_t last = static_cast<uint32_t>(arr[31]);
    const int32_t l0 = base;
    const int32_t l1 = wrapping_add(l0, static_cast<int32_t>((last >> 24) & 255u));
    const int32_t l2 = wrapping_add(l1, static_cast<int32_t>((last >> 16) & 255u));
    const int32_t l3 = wrapping_add(l2, static_cast<int32_t>((last >> 8) & 255u));

    for (size_t i = 0; i < 32; ++i) {
        const uint32_t lanes = static_cast<uint32_t>(arr[i]);
        arr[i] = wrapping_add(static_cast<int32_t>((lanes >> 24) & 255u), l0);
        arr[32 + i] = wrapping_add(static_cast<int32_t>((lanes >> 16) & 255u), l1);
        arr[64 + i] = wrapping_add(static_cast<int32_t>((lanes >> 8) & 255u), l2);
        arr[96 + i] = wrapping_add(static_cast<int32_t>(lanes & 255u), l3);
    }
}

void ForDeltaUtil::prefix_sum16(std::span<int32_t> arr, int32_t base) {
    debug_check(arr.size() >= BLOCK_SIZE, "prefix_sum16 requires 128 values");
    // Values use at most 10 bits, so summing 64 of them in the 16-bit lanes
    // cannot carry into the neighbour lane.
    prefix_sum_small(arr.first(64), 64);

    const uint32_t last = static_cast<uint32_t>(arr[63]);
    const int32_t l0 = base;
    const int32_t l1 = wrapping_add(l0, static_cast<int32_t>((last >> 16) & 65535u));

    for (size_t i = 0; i < 64; ++i) {
        const uint32_t lanes = static_cast<uint32_t>(arr[i]);
        arr[i] = wrapping_add(static_cast<int32_t>((lanes >> 16) & 65535u), l0);
        arr[64 + i] = wrapping_add(static_cast<int32_t>(lanes & 65535u), l1);
    }
}

void ForDeltaUtil::prefix_sum32(std::span<int32_t> arr, int32_t base) {
    debug_check(arr.size() >= BLOCK_SIZE, "prefix_sum32 requires 128 values");
    int32_t carry = base;
    for (size_t i = 0; i < BLOCK_SIZE; ++i) {
        carry = wrapping_add(arr[i], carry);
        arr[i] = carry;
    }
}

VoidResult ForDeltaUtil::encode_deltas(std::span<int32_t> ints, DataOutput &out) {
    if (ints.size() < BLOCK_SIZE) {
        return err(LuceneError::illegal_argument("encode_deltas requires 128 values"));
    }
    if (ints[0] == 1 && PForUtil::all_equal(ints)) {
        return out.write_byte(0);
    }

    int32_t or_value = 0;
    for (size_t i = 0; i < BLOCK_SIZE; ++i) {
        or_value |= ints[i];
    }
    debug_check(or_value != 0, "deltas must not be all zero unless they are all ones");

    auto bits = PackedInts::bits_required(static_cast<int64_t>(or_value));
    if (!bits) {
        return std::unexpected(std::move(bits.error()));
    }
    const int32_t bits_per_value = bits.value();
    auto written = out.write_byte(static_cast<uint8_t>(bits_per_value));
    if (!written) {
        return written;
    }

    int32_t primitive_size = 32;
    if (bits_per_value <= 3) {
        ForUtil::collapse8(ints);
        primitive_size = 8;
    } else if (bits_per_value <= 10) {
        ForUtil::collapse16(ints);
        primitive_size = 16;
    }

    return ForUtil::encode_with_tmp(ints, bits_per_value, primitive_size, out, tmp_);
}

VoidResult ForDeltaUtil::decode_and_prefix_sum(PostingDecodingUtil &pdu, int32_t base, std::span<int32_t> ints) {
    if (ints.size() < BLOCK_SIZE) {
        return err(LuceneError::illegal_argument("decode_and_prefix_sum requires 128 values"));
    }
    auto token = pdu.read_byte();
    if (!token) {
        return std::unexpected(std::move(token.error()));
    }
    const int32_t bits_per_value = token.value();
    if (bits_per_value == 0) {
        prefix_sum_of_ones(ints, base);
        return {};
    }
    return decode_and_prefix_sum_with_bits(bits_per_value, pdu, base, ints);
}

VoidResult ForDeltaUtil::decode_and_prefix_sum_with_bits(int32_t bits_per_value,
                                                        PostingDecodingUtil &pdu,
                                                        int32_t base,
                                                        std::span<int32_t> ints) {
    VoidResult decoded;
    switch (bits_per_value) {
        case 1:
            decoded = ForUtil::decode1(pdu, ints);
            if (decoded) {
                prefix_sum8(ints, base);
            }
            return decoded;
        case 2:
            decoded = ForUtil::decode2(pdu, ints);
            if (decoded) {
                prefix_sum8(ints, base);
            }
            return decoded;
        case 3:
            decoded = ForUtil::decode3(pdu, tmp_, ints);
            if (decoded) {
                prefix_sum8(ints, base);
            }
            return decoded;
        case 4:
            decoded = decode_4_to_16(pdu, ints);
            if (decoded) {
                prefix_sum16(ints, base);
            }
            return decoded;
        case 5:
            decoded = decode_5_to_16(pdu, tmp_, ints);
            if (decoded) {
                prefix_sum16(ints, base);
            }
            return decoded;
        case 6:
            decoded = decode_6_to_16(pdu, tmp_, ints);
            if (decoded) {
                prefix_sum16(ints, base);
            }
            return decoded;
        case 7:
            decoded = decode_7_to_16(pdu, tmp_, ints);
            if (decoded) {
                prefix_sum16(ints, base);
            }
            return decoded;
        case 8:
            decoded = decode_8_to_16(pdu, ints);
            if (decoded) {
                prefix_sum16(ints, base);
            }
            return decoded;
        case 9:
            decoded = ForUtil::decode9(pdu, tmp_, ints);
            if (decoded) {
                prefix_sum16(ints, base);
            }
            return decoded;
        case 10:
            decoded = ForUtil::decode10(pdu, tmp_, ints);
            if (decoded) {
                prefix_sum16(ints, base);
            }
            return decoded;
        case 11:
            decoded = decode_11_to_32(pdu, tmp_, ints);
            if (decoded) {
                prefix_sum32(ints, base);
            }
            return decoded;
        case 12:
            decoded = decode_12_to_32(pdu, tmp_, ints);
            if (decoded) {
                prefix_sum32(ints, base);
            }
            return decoded;
        case 13:
            decoded = decode_13_to_32(pdu, tmp_, ints);
            if (decoded) {
                prefix_sum32(ints, base);
            }
            return decoded;
        case 14:
            decoded = decode_14_to_32(pdu, tmp_, ints);
            if (decoded) {
                prefix_sum32(ints, base);
            }
            return decoded;
        case 15:
            decoded = decode_15_to_32(pdu, tmp_, ints);
            if (decoded) {
                prefix_sum32(ints, base);
            }
            return decoded;
        case 16:
            decoded = decode_16_to_32(pdu, ints);
            if (decoded) {
                prefix_sum32(ints, base);
            }
            return decoded;
        default:
            decoded = ForUtil::decode_slow(bits_per_value, pdu, tmp_, ints);
            if (decoded) {
                prefix_sum32(ints, base);
            }
            return decoded;
    }
}

} // namespace lucene
