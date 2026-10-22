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

export module lucene_core:for_delta_util;

import :types;
import std;

import :error;
import :data_output;
import :for_util;
import :posting_decoding_util;

namespace lucene {

/// Frame-of-reference delta bit packing for the Lucene101 postings format.
/// Port of `ForDeltaUtil` (`core/codecs/lucene101/for_delta_util.rs`).
///
/// Deltas of a strictly increasing sequence are bit-packed and, on decode,
/// turned back into absolute values by a prefix sum that adds `base`.  Four
/// 8-bit lanes per integer are used for `bits_per_value <= 3`, two 16-bit
/// lanes for `bits_per_value <= 10`, and full-width integers otherwise — so
/// the prefix sum can be computed on whole lanes without carry propagation
/// between them.
export class ForDeltaUtil {
public:
    static constexpr size_t BLOCK_SIZE = ForUtil::BLOCK_SIZE;
    static constexpr size_t HALF_BLOCK_SIZE = BLOCK_SIZE / 2;

    ForDeltaUtil() : tmp_(BLOCK_SIZE, 0) {}

    /// Encodes the deltas in `ints` (128 values, strictly positive when the
    /// sequence is strictly increasing).  `ints` is modified in place: the
    /// lane layout is collapsed for the narrower widths.
    [[nodiscard]] VoidResult encode_deltas(std::span<int32_t> ints, DataOutput &out);

    /// Decodes 128 deltas, computes the prefix sum starting at `base` and
    /// leaves the 128 absolute values in `ints`.
    [[nodiscard]] VoidResult decode_and_prefix_sum(PostingDecodingUtil &pdu, int32_t base, std::span<int32_t> ints);

    /// Prefix sum of the deltas of an all-ones block: `arr[i] = base + i + 1`.
    static void prefix_sum_of_ones(std::span<int32_t> arr, int32_t base);
    /// Prefix sum of a block whose deltas were packed in 8-bit lanes.
    static void prefix_sum8(std::span<int32_t> arr, int32_t base);
    /// Prefix sum of a block whose deltas were packed in 16-bit lanes.
    static void prefix_sum16(std::span<int32_t> arr, int32_t base);
    /// Prefix sum of a block whose deltas were packed in whole integers.
    static void prefix_sum32(std::span<int32_t> arr, int32_t base);

private:
    [[nodiscard]] VoidResult decode_and_prefix_sum_with_bits(int32_t bits_per_value,
                                                            PostingDecodingUtil &pdu,
                                                            int32_t base,
                                                            std::span<int32_t> ints);

    std::vector<int32_t> tmp_;
};

} // namespace lucene
