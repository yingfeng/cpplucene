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
/// rlucene has `wide`-based SIMD fast paths in both methods.  The port keeps
/// the scalar reference semantics only: the specialised branches decode the
/// same bits into the same planes, they are just structured for automatic
/// vectorisation.  clang vectorises the loops below on the hot sizes
/// (`count` is always a multiple of 4 in the postings code).
export class PostingDecodingUtil {
public:
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
