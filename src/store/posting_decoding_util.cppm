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

import :data_input;
import :error;
import :memory_segment_index_input;

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

    explicit PostingDecodingUtil(DataInput &input)
        : input_(input),
          // Resolved once: the postings readers always hand in a memory mapped
          // input, and reading it through the concrete type turns the per-byte
          // virtual calls of the decoders into inlined loads.
          concrete_(dynamic_cast<MemorySegmentIndexInput *>(&input)) {}

    [[nodiscard]] DataInput &input() noexcept { return input_; }

    /// One byte, from the concrete input when there is one.
    [[gnu::always_inline]] [[nodiscard]] Result<uint8_t> read_byte() {
        if (concrete_ != nullptr) {
            return concrete_->read_byte();
        }
        return input_.read_byte();
    }

    [[gnu::always_inline]] [[nodiscard]] Result<int16_t> read_short() {
        if (concrete_ != nullptr) {
            return concrete_->read_short();
        }
        return input_.read_short();
    }

    [[gnu::always_inline]] [[nodiscard]] Result<int32_t> read_vint() {
        if (concrete_ != nullptr) {
            return concrete_->read_vint();
        }
        return input_.read_vint();
    }

    [[gnu::always_inline]] [[nodiscard]] Result<int64_t> read_vlong() {
        if (concrete_ != nullptr) {
            return concrete_->read_vlong();
        }
        return input_.read_vlong();
    }

    [[gnu::always_inline]] [[nodiscard]] Result<int32_t> read_int() {
        if (concrete_ != nullptr) {
            return concrete_->read_int();
        }
        return input_.read_int();
    }

    [[gnu::always_inline]] [[nodiscard]] Result<int64_t> read_long() {
        if (concrete_ != nullptr) {
            return concrete_->read_long();
        }
        return input_.read_long();
    }

    [[gnu::always_inline]] [[nodiscard]] VoidResult read_bytes(std::span<uint8_t> bytes) {
        if (concrete_ != nullptr) {
            return concrete_->read_bytes(bytes);
        }
        return input_.read_bytes(bytes);
    }

    [[gnu::always_inline]] [[nodiscard]] VoidResult read_ints(std::span<int32_t> dst, size_t offset, size_t len) {
        if (concrete_ != nullptr) {
            return concrete_->read_ints(dst, offset, len);
        }
        return input_.read_ints(dst, offset, len);
    }

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
    MemorySegmentIndexInput *concrete_;
};

} // namespace lucene
