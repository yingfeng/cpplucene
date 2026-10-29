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

module;

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

module lucene_core:ut.posting_decoding_util;

import lucene_core;

using namespace lucene;

namespace {

/// `count` pseudo-random values that exercise every bit, so a wrong shift or a
/// missing mask shows up immediately.
[[nodiscard]] std::vector<int32_t> make_input(size_t count, uint32_t seed) {
    std::vector<int32_t> values(count);
    uint32_t state = seed;
    for (size_t i = 0; i < count; ++i) {
        state = state * 1664525u + 1013904223u;
        values[i] = static_cast<int32_t>(state);
    }
    return values;
}

[[nodiscard]] std::vector<uint8_t> encode_ints(const std::vector<int32_t> &values) {
    std::vector<uint8_t> bytes(values.size() * 4, 0);
    ByteArrayDataOutput out{std::span<uint8_t>(bytes)};
    for (int32_t value : values) {
        EXPECT_TRUE(out.write_int(value).has_value());
    }
    return bytes;
}

/// `value >> shift` with the reference implementation's semantics: shifts of 32
/// or more yield 0 rather than being undefined.
[[nodiscard]] uint32_t shift_out(uint32_t value, int32_t shift) {
    return shift >= 32 ? 0u : (value >> static_cast<uint32_t>(shift));
}

/// The scalar model of `split_ints_same`, taken straight from the documented
/// contract: read `count` values into `c`, then for every plane `j` with a
/// positive shift store `(value >> shift) & b_mask` at `count * j`, and finally
/// keep only the low `c_mask` bits of `c`.
[[nodiscard]] std::vector<int32_t> model_same(size_t count,
                                              const std::vector<int32_t> &input,
                                              int32_t b_shift,
                                              int32_t dec,
                                              int32_t b_mask,
                                              size_t c_index,
                                              int32_t c_mask,
                                              size_t buffer_size) {
    std::vector<int32_t> expected(buffer_size, 0);
    const int32_t max_iter = (b_shift - 1) / dec;
    for (size_t i = 0; i < count; ++i) {
        const uint32_t value = static_cast<uint32_t>(input[i]);
        for (int32_t j = 0; j <= max_iter; ++j) {
            const int32_t shift = b_shift - j * dec;
            if (shift > 0) {
                expected[count * static_cast<size_t>(j) + i] =
                        static_cast<int32_t>(shift_out(value, shift) & static_cast<uint32_t>(b_mask));
            }
        }
        expected[c_index + i] = input[i] & c_mask;
    }
    return expected;
}

[[nodiscard]] std::vector<int32_t> model_diff(size_t count,
                                              const std::vector<int32_t> &input,
                                              int32_t b_shift,
                                              int32_t dec,
                                              int32_t b_mask,
                                              int32_t c_mask,
                                              size_t b_size) {
    std::vector<int32_t> expected(b_size, 0);
    const int32_t max_iter = (b_shift - 1) / dec;
    for (size_t i = 0; i < count; ++i) {
        const uint32_t value = static_cast<uint32_t>(input[i]);
        for (int32_t j = 0; j <= max_iter; ++j) {
            const int32_t shift = b_shift - j * dec;
            if (shift > 0) {
                expected[count * static_cast<size_t>(j) + i] =
                        static_cast<int32_t>(shift_out(value, shift) & static_cast<uint32_t>(b_mask));
            }
        }
    }
    return expected;
}

/// One `split_ints_same` layout, checked against the scalar model.
void check_same(size_t count,
                int32_t b_shift,
                int32_t dec,
                int32_t b_mask,
                size_t c_index,
                int32_t c_mask,
                size_t buffer_size) {
    SCOPED_TRACE(::testing::Message() << "count=" << count << " b_shift=" << b_shift << " dec=" << dec
                                      << " c_index=" << c_index);
    const std::vector<int32_t> input = make_input(count, static_cast<uint32_t>(count * 31 + b_shift));
    const std::vector<uint8_t> bytes = encode_ints(input);

    ByteArrayDataInput in{std::span<const uint8_t>(bytes)};
    PostingDecodingUtil pdu(in);
    std::vector<int32_t> actual(buffer_size, 0);
    ASSERT_TRUE(pdu.split_ints_same(count, actual, b_shift, dec, b_mask, c_index, c_mask).has_value());

    const std::vector<int32_t> expected =
            model_same(count, input, b_shift, dec, b_mask, c_index, c_mask, buffer_size);
    EXPECT_EQ(actual, expected);
}

/// One `split_ints_diff` layout, checked against the scalar model.  `c` is not
/// part of the expectation except for its mask, which is checked separately.
void check_diff(size_t count,
                int32_t b_shift,
                int32_t dec,
                int32_t b_mask,
                int32_t c_mask,
                size_t b_size) {
    SCOPED_TRACE(::testing::Message() << "count=" << count << " b_shift=" << b_shift << " dec=" << dec);
    const std::vector<int32_t> input = make_input(count, static_cast<uint32_t>(count * 17 + dec));
    std::vector<uint8_t> bytes = encode_ints(input);
    const std::vector<int32_t> raw = input;

    ByteArrayDataInput in{std::span<const uint8_t>(bytes)};
    PostingDecodingUtil pdu(in);
    std::vector<int32_t> b(b_size, 0);
    std::vector<int32_t> c(b_size, 0);
    ASSERT_TRUE(pdu.split_ints_diff(count, b, b_shift, dec, b_mask, c, 0, c_mask).has_value());

    const std::vector<int32_t> expected = model_diff(count, raw, b_shift, dec, b_mask, c_mask, b_size);
    EXPECT_EQ(b, expected);
    for (size_t i = 0; i < count; ++i) {
        EXPECT_EQ(c[i], raw[i] & c_mask) << "c[" << i << "]";
    }
}

} // namespace

TEST(PostingDecodingUtilTest, VectorKernelsAreCompiledInWhereSupported) {
#if defined(__x86_64__) || defined(__aarch64__)
    EXPECT_TRUE(PostingDecodingUtil::has_vector_kernels);
#else
    EXPECT_FALSE(PostingDecodingUtil::has_vector_kernels);
#endif
}

TEST(PostingDecodingUtilTest, SplitIntsSameMatchesTheScalarModel) {
    // The two fixed-shape fast paths of the reference implementation, with the
    // exact arguments `ForUtil::decode8` / `decode16` use.
    check_same(4, 7, 1, 0xFF, 28, 0xFF, 32);
    check_same(8, 6, 2, 0xFFFF, 24, 0xFFFF, 32);
    // 16 inputs over 4 planes with a single output plane (`ForUtil::decode4`).
    check_same(16, 4, 4, 0xFF, 16, 0xFF, 32);
    // Delta decoders: one plane (16/32/64 inputs)...
    check_same(16, 12, 4, 0xFFFF, 48, 0xFFFF, 64);
    check_same(32, 8, 8, 0xFFFF, 32, 0xFFFF, 64);
    check_same(64, 16, 16, 0xFFFF, 64, 0xFFFF, 128);
    // ... and three planes (`max_iter == 2`) with a 128-value block.
    check_same(16, 9, 4, 0xFF, 48, 0xFF, 64);
    // Two planes from a 128-value block.
    check_same(64, 9, 8, 0xFF, 128, 0xFF, 256);
    // Counts that are not a multiple of four, i.e. the scalar tail.
    check_same(5, 9, 4, 0xFF, 15, 0xFF, 32);
    check_same(7, 12, 4, 0xFFFF, 21, 0xFFFF, 32);
}

TEST(PostingDecodingUtilTest, SplitIntsSameHandlesManyPlanes) {
    // `max_iter >= 3` takes the generic vector-at-a-time path.
    check_same(24, 24, 6, 0xFF, 96, 0xFF, 128);
    check_same(32, 24, 5, 0xFF, 224, 0xFF, 256);
    check_same(12, 16, 4, 0xFFFF, 60, 0xFFFF, 72);
    check_same(9, 13, 3, 0xFF, 45, 0xFF, 64);
}

TEST(PostingDecodingUtilTest, SplitIntsDiffMatchesTheScalarModel) {
    check_diff(4, 7, 1, 0xFF, 0xFF, 32);
    check_diff(8, 6, 2, 0xFFFF, 0xFFFF, 32);
    check_diff(16, 4, 4, 0xFF, 0xFF, 32);
    // The real shapes of `ForUtil::decodeN` (`count == bits_per_value * 4`).
    check_diff(20, 24, 32, 0xFF, -1, 128);
    check_diff(28, 1, 7, 0xFF, 0xFF, 128);
    check_diff(44, 16, 32, 0xFFFF, -1, 128);
    check_diff(64, 1, 16, 0xFFFF, 0xFFFF, 128);
    // Multi-plane and scalar-tail layouts.
    check_diff(16, 9, 4, 0xFF, 0xFF, 64);
    check_diff(32, 8, 8, 0xFF, 0xFF, 128);
    check_diff(6, 9, 4, 0xFF, 0xFF, 32);
    check_diff(5, 24, 6, 0xFF, -1, 128);
}

TEST(PostingDecodingUtilTest, RejectsNonPositiveDecrement) {
    const std::vector<int32_t> input = make_input(4, 1);
    const std::vector<uint8_t> bytes = encode_ints(input);

    ByteArrayDataInput in{std::span<const uint8_t>(bytes)};
    PostingDecodingUtil pdu(in);
    std::vector<int32_t> same(32, 0);
    auto failed = pdu.split_ints_same(4, same, 7, 0, 0xFF, 28, 0xFF);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().kind(), ErrorKind::kIllegalArgument);
    EXPECT_EQ(in.get_position(), 0U) << "the decrement is validated before reading";

    std::vector<int32_t> b(32, 0);
    std::vector<int32_t> c(32, 0);
    ByteArrayDataInput in2{std::span<const uint8_t>(bytes)};
    PostingDecodingUtil pdu2(in2);
    auto failed2 = pdu2.split_ints_diff(4, b, 7, -1, 0xFF, c, 0, 0xFF);
    ASSERT_FALSE(failed2.has_value());
    EXPECT_EQ(failed2.error().kind(), ErrorKind::kIllegalArgument);
    EXPECT_EQ(in2.get_position(), 0U);
}

TEST(PostingDecodingUtilTest, PropagatesTruncatedInput) {
    const std::vector<int32_t> input = make_input(4, 3);
    const std::vector<uint8_t> bytes = encode_ints(input);

    ByteArrayDataInput in{std::span<const uint8_t>(bytes)};
    PostingDecodingUtil pdu(in);
    // Asks for eight integers but only four are available.
    std::vector<int32_t> buffer(64, 0);
    auto failed = pdu.split_ints_same(8, buffer, 6, 2, 0xFFFF, 24, 0xFFFF);
    EXPECT_FALSE(failed.has_value());
}
