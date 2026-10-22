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

export module lucene_core:for_util;

import :types;
import std;

import :error;
import :data_output;
import :posting_decoding_util;

namespace lucene {

/// Frame-of-reference bit packing for the Lucene101 postings format.
/// Port of `ForUtil` (`core/codecs/lucene101/for_util.rs`).
///
/// Inspired by <https://fulmicoton.com/posts/bitpacking/>: several integers
/// are packed into one `int32_t` to achieve SIMD-like speedups.
///
///  - if `bits_per_value <= 8`, 4 integers are packed into each `int32_t`
///    (byte lanes),
///  - if `bits_per_value <= 16`, 2 integers per `int32_t` (short lanes),
///  - otherwise one integer per `int32_t`.
///
/// Deviation from the reference implementation: the hand-unrolled `decode1` .. `decode16` SIMD
/// kernels and the `bits_per_value == 18` packed-tail special case of the
/// encoder are not ported.  They only reshuffle the same bits into the same
/// planes; [`decode`] therefore always uses the scalar reference algorithm
/// ([`decode_slow`]) and still applies `expand8`/`expand16`.  The on-disk
/// format is unchanged because the layout produced by the generic encoder is
/// identical.
export class ForUtil {
public:
    static constexpr size_t BLOCK_SIZE = 128;
    static constexpr size_t BLOCK_SIZE_LOG2 = 7;

    ForUtil() : tmp_(BLOCK_SIZE, 0) {}

    // ---- lane (un)packing helpers ----

    /// Spreads the 32 byte lanes of `arr[0..32]` over `arr[0..128]`.
    static void expand8(std::span<int32_t> arr);
    /// Packs `arr[0..128]` back into the 32 byte lanes of `arr[0..32]`.
    static void collapse8(std::span<int32_t> arr);
    /// Spreads the 64 short lanes of `arr[0..64]` over `arr[0..128]`.
    static void expand16(std::span<int32_t> arr);
    /// Packs `arr[0..128]` back into the 64 short lanes of `arr[0..64]`.
    static void collapse16(std::span<int32_t> arr);

    // ---- encode / decode ----

    /// Encodes the 128 integers of `ints` into `out`, collapsing the lanes
    /// first when `bits_per_value <= 16`.  `ints` is modified in place.
    [[nodiscard]] VoidResult encode(std::span<int32_t> ints, int32_t bits_per_value, DataOutput &out);

    /// Encode 128 integers assuming the lanes have already been collapsed to
    /// `primitive_size` bits.
    [[nodiscard]] static VoidResult encode_with_tmp(std::span<const int32_t> ints,
                                                    int32_t bits_per_value,
                                                    int32_t primitive_size,
                                                    DataOutput &out,
                                                    std::span<int32_t> tmp);

    /// Number of bytes required to encode 128 integers of `bits_per_value`
    /// bits per value.
    [[nodiscard]] static constexpr int32_t num_bytes(int32_t bits_per_value) {
        return bits_per_value << (BLOCK_SIZE_LOG2 - 3);
    }

    /// Scalar reference decoder: reads `bits_per_value * 4` packed integers
    /// into `tmp` and unpacks all 128 values into `ints` (still in the
    /// `primitive_size` lane layout, i.e. `expand8`/`expand16` may be needed).
    [[nodiscard]] static VoidResult decode_slow(int32_t bits_per_value,
                                                PostingDecodingUtil &pdu,
                                                std::span<int32_t> tmp,
                                                std::span<int32_t> ints);

    /// Decodes 128 values and applies the lane expansion matching
    /// `bits_per_value`.
    [[nodiscard]] VoidResult decode(int32_t bits_per_value, PostingDecodingUtil &pdu, std::span<int32_t> ints);

    // ---- specialised decoders ----
    //
    // One entry per bit width.  Unlike `decode`, they leave the values in the
    // lane layout (`ForDeltaUtil` exploits that by prefix-summing whole lanes).
    // They are public because `ForUtil` and `ForDeltaUtil` live in different
    // translation units (the reference implementation can use `pub(crate)`).

    [[nodiscard]] static VoidResult decode1(PostingDecodingUtil &pdu, std::span<int32_t> ints);
    [[nodiscard]] static VoidResult decode2(PostingDecodingUtil &pdu, std::span<int32_t> ints);
    [[nodiscard]] static VoidResult decode3(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints);
    [[nodiscard]] static VoidResult decode4(PostingDecodingUtil &pdu, std::span<int32_t> ints);
    [[nodiscard]] static VoidResult decode5(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints);
    [[nodiscard]] static VoidResult decode6(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints);
    [[nodiscard]] static VoidResult decode7(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints);
    [[nodiscard]] static VoidResult decode8(PostingDecodingUtil &pdu, std::span<int32_t> ints);
    [[nodiscard]] static VoidResult decode9(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints);
    [[nodiscard]] static VoidResult decode10(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints);
    [[nodiscard]] static VoidResult decode11(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints);
    [[nodiscard]] static VoidResult decode12(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints);
    [[nodiscard]] static VoidResult decode13(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints);
    [[nodiscard]] static VoidResult decode14(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints);
    [[nodiscard]] static VoidResult decode15(PostingDecodingUtil &pdu, std::span<int32_t> tmp, std::span<int32_t> ints);
    [[nodiscard]] static VoidResult decode16(PostingDecodingUtil &pdu, std::span<int32_t> ints);

    // ---- masks ----

    static constexpr int32_t mask32(int32_t bits_per_value) {
        return bits_per_value >= 32 ? -1
                                    : static_cast<int32_t>((1u << static_cast<uint32_t>(bits_per_value)) - 1u);
    }
    static constexpr int32_t expand_mask16(int32_t mask16) {
        return static_cast<int32_t>(static_cast<uint32_t>(mask16) | (static_cast<uint32_t>(mask16) << 16));
    }
    static constexpr int32_t expand_mask8(int32_t mask8) {
        return expand_mask16(static_cast<int32_t>(static_cast<uint32_t>(mask8) | (static_cast<uint32_t>(mask8) << 8)));
    }
    static constexpr int32_t mask16(int32_t bits_per_value) {
        return expand_mask16(static_cast<int32_t>((1u << static_cast<uint32_t>(bits_per_value)) - 1u));
    }
    static constexpr int32_t mask8(int32_t bits_per_value) {
        return expand_mask8(static_cast<int32_t>((1u << static_cast<uint32_t>(bits_per_value)) - 1u));
    }

    /// `MASKS{8,16,32}[bits]`, resolved through the lane width of the layout.
    [[nodiscard]] static constexpr int32_t mask_for(int32_t primitive_size, size_t bits) {
        switch (primitive_size) {
            case 8:
                return mask8(static_cast<int32_t>(bits));
            case 16:
                return mask16(static_cast<int32_t>(bits));
            default:
                return mask32(static_cast<int32_t>(bits));
        }
    }

private:
    /// Scratch buffer of [`BLOCK_SIZE`] values, reused across decodes.
    std::vector<int32_t> tmp_;
};

} // namespace lucene
