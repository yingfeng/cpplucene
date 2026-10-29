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

export module lucene_core:posting_decoding_util;

import :types;
import std;

import :error;
import :data_input;

namespace lucene {

/// Utility struct to decode postings.  Port of `PostingDecodingUtil`
/// (`core/internal/vectorization/posting_decoding_util.rs`).
///
/// The reference implementation has `wide`-based SIMD fast paths in both
/// methods: fixed-shape specialisations (4 inputs over 7 planes, 8 inputs over 3
/// planes), a single-plane pass, and a vector-at-a-time loop over the remaining
/// plane layouts.  This port implements the same branches with clang's native
/// vector types (`ext_vector_type`), which map onto SSE2/AVX2 on x86-64 and NEON
/// on aarch64.  Both ISAs have 128-bit vectors in their base set, so the fast
/// paths need no runtime dispatch.  On any other architecture the scalar kernel
/// is used; see [`has_vector_kernels`](#has_vector_kernels).
export class PostingDecodingUtil {
public:
    /// Whether the vectorised kernels are compiled in for this target.
#if defined(__x86_64__) || defined(__aarch64__)
    static constexpr bool has_vector_kernels = true;
#else
    static constexpr bool has_vector_kernels = false;
#endif

    explicit PostingDecodingUtil(DataInput &input) : input_(input) {}

    [[nodiscard]] DataInput &input() noexcept { return input_; }

    /// Core method for decoding blocks of docs / freqs / positions / offsets:
    ///
    ///  - read `count` integers into `b_and_c[c_index..]`
    ///  - for all `j >= 0` such that `b_shift - j * dec > 0`:
    ///      apply shift `b_shift - j * dec` to each value in `c` and store the
    ///      result in `b` at offset `count * j`
    ///  - apply mask `c_mask` to each value in `c` starting at `c_index`
    [[nodiscard]] VoidResult split_ints_same(size_t count,
                                             std::span<int32_t> b_and_c,
                                             int32_t b_shift,
                                             int32_t dec,
                                             int32_t b_mask,
                                             size_t c_index,
                                             int32_t c_mask);

    /// Same as [`split_ints_same`] but the `b` and `c` planes live in
    /// different arrays.
    [[nodiscard]] VoidResult split_ints_diff(size_t count,
                                             std::span<int32_t> b,
                                             int32_t b_shift,
                                             int32_t dec,
                                             int32_t b_mask,
                                             std::span<int32_t> c,
                                             size_t c_index,
                                             int32_t c_mask);

private:
    DataInput &input_;
};

} // namespace lucene
