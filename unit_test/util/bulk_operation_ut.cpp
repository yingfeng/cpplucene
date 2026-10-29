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

module lucene_core:ut.bulk_operation;

import lucene_core;

using namespace lucene;

namespace {

[[nodiscard]] uint64_t mask_for(int32_t bits) {
    return bits >= 64 ? ~uint64_t{0} : ((uint64_t{1} << static_cast<uint32_t>(bits)) - 1);
}

[[nodiscard]] std::vector<int64_t> make_values(size_t count, int32_t bits, uint32_t seed) {
    std::vector<int64_t> values(count, 0);
    uint64_t state = seed;
    const uint64_t mask = mask_for(bits);
    for (size_t i = 0; i < count; ++i) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        values[i] = static_cast<int64_t>((state >> 7) & mask);
    }
    return values;
}

/// Scalar model of the `Format.PACKED` layout: values are laid out most
/// significant bit first, `ceil(count * bits / 8)` bytes in total.
[[nodiscard]] int64_t oracle_value_at(std::span<const uint8_t> bytes, int32_t bits, size_t index) {
    uint64_t value = 0;
    for (int32_t b = 0; b < bits; ++b) {
        const size_t bit = index * static_cast<size_t>(bits) + static_cast<size_t>(b);
        const uint8_t byte = bytes[bit >> 3];
        const auto bit_value = static_cast<uint64_t>((byte >> (7 - (bit & 7))) & 1);
        value = (value << 1) | bit_value;
    }
    return static_cast<int64_t>(value);
}

/// Encodes `values` with the scalar model, for the read direction.
[[nodiscard]] std::vector<uint8_t> oracle_encode(std::span<const int64_t> values, int32_t bits) {
    std::vector<uint8_t> bytes((values.size() * static_cast<size_t>(bits) + 7) / 8, 0);
    for (size_t i = 0; i < values.size(); ++i) {
        const auto value = static_cast<uint64_t>(values[i]);
        for (int32_t b = 0; b < bits; ++b) {
            const auto bit_value = static_cast<uint8_t>((value >> static_cast<uint32_t>(bits - 1 - b)) & 1);
            const size_t bit = i * static_cast<size_t>(bits) + static_cast<size_t>(b);
            bytes[bit >> 3] |= static_cast<uint8_t>(bit_value << (7 - (bit & 7)));
        }
    }
    return bytes;
}

/// Full-group round trip for one bit width, through both the byte and the
/// 64-bit-block entry points.
void check_roundtrip(int32_t bits) {
    SCOPED_TRACE(::testing::Message() << "bits=" << bits);
    auto codec = bulk_operation(bits);
    ASSERT_TRUE(codec.has_value());
    const BulkOperation &operation = codec.value().operation();
    ASSERT_EQ(operation.bits_per_value(), bits);

    const size_t values_per_group = operation.byte_value_count();
    ASSERT_GT(values_per_group, 0U);
    const size_t groups = 3;
    const size_t count = values_per_group * groups;
    const std::vector<int64_t> values = make_values(count, bits, static_cast<uint32_t>(bits) * 7 + 1);

    // ---- byte layout ----
    std::vector<uint8_t> bytes((count * static_cast<size_t>(bits) + 7) / 8, 0);
    ASSERT_TRUE(codec.value().encode(values, 0, bytes).has_value());
    std::vector<int64_t> decoded(count, 0);
    ASSERT_TRUE(codec.value().decode(bytes, 0, decoded).has_value());
    EXPECT_EQ(decoded, values);

    // ---- the same bytes must match the scalar model ----
    for (size_t i = 0; i < count; ++i) {
        ASSERT_EQ(oracle_value_at(bytes, bits, i), values[i]) << "value " << i;
    }
    EXPECT_EQ(oracle_encode(values, bits), bytes);

    // ---- 64-bit block layout ----
    // The block layout only lines up on whole `long_value_count` groups.
    const size_t block_values = operation.long_value_count() * groups;
    const std::vector<int64_t> block_input = make_values(block_values, bits, static_cast<uint32_t>(bits) * 13 + 5);
    std::vector<uint64_t> blocks(operation.long_block_count() * groups, 0);
    operation.encode_i64_to_u64(block_input, 0, blocks, 0, groups);
    std::vector<int64_t> from_blocks(block_values, 0);
    operation.decode_u64_to_i64(blocks, 0, from_blocks, 0, groups);
    EXPECT_EQ(from_blocks, block_input);
}

} // namespace

TEST(BulkOperationTest, FactoryCoversEveryPackedWidth) {
    for (int32_t bits = 1; bits <= 64; ++bits) {
        auto codec = bulk_operation(bits);
        ASSERT_TRUE(codec.has_value()) << "bits=" << bits;
        EXPECT_EQ(codec.value().operation().bits_per_value(), bits);
    }

    auto too_small = bulk_operation(0);
    ASSERT_FALSE(too_small.has_value());
    EXPECT_EQ(too_small.error().kind(), ErrorKind::kArrayIndexOutOfBounds);
    auto too_large = bulk_operation(65);
    ASSERT_FALSE(too_large.has_value());
    EXPECT_EQ(too_large.error().kind(), ErrorKind::kArrayIndexOutOfBounds);
}

TEST(BulkOperationTest, BlockAndValueCountsMatchTheReferenceFormulas) {
    struct Expected {
        int32_t bits;
        size_t long_blocks;
        size_t long_values;
        size_t byte_blocks;
        size_t byte_values;
    };
    // The examples the reference implementation documents in `BulkOperation`.
    const Expected cases[] = {
            {1, 1, 64, 1, 8},
            {12, 3, 16, 3, 2},
            {16, 1, 4, 2, 1},
            {50, 25, 32, 25, 4},
            {63, 63, 64, 63, 8},
            {64, 1, 1, 8, 1},
    };
    for (const Expected &expected : cases) {
        auto codec = bulk_operation(expected.bits);
        ASSERT_TRUE(codec.has_value());
        const BulkOperation &operation = codec.value().operation();
        SCOPED_TRACE(::testing::Message() << "bits=" << expected.bits);
        EXPECT_EQ(operation.long_block_count(), expected.long_blocks);
        EXPECT_EQ(operation.long_value_count(), expected.long_values);
        EXPECT_EQ(operation.byte_block_count(), expected.byte_blocks);
        EXPECT_EQ(operation.byte_value_count(), expected.byte_values);
        // A group holds a whole number of bits.
        EXPECT_EQ(operation.byte_value_count() * static_cast<size_t>(expected.bits), operation.byte_block_count() * 8);
    }
}

TEST(BulkOperationTest, RoundTripsEveryBitWidth) {
    for (int32_t bits = 1; bits <= 64; ++bits) {
        check_roundtrip(bits);
    }
}

TEST(BulkOperationTest, RoundTripsPartialGroups) {
    for (const int32_t bits : {1, 2, 3, 5, 7, 8, 11, 12, 13, 16, 21, 25, 32, 33, 48, 63}) {
        SCOPED_TRACE(::testing::Message() << "bits=" << bits);
        auto codec = bulk_operation(bits);
        ASSERT_TRUE(codec.has_value());
        const size_t values_per_group = codec.value().operation().byte_value_count();
        if (values_per_group < 2) {
            // Every value is its own group, so there is no partial group.
            continue;
        }
        // Two whole groups plus a partial one, so the scratch-buffer path runs.
        const size_t count = 2 * values_per_group + values_per_group / 2;
        const std::vector<int64_t> values = make_values(count, bits, static_cast<uint32_t>(bits));

        std::vector<uint8_t> bytes((count * static_cast<size_t>(bits) + 7) / 8, 0);
        ASSERT_TRUE(codec.value().encode(values, 0, bytes).has_value());
        std::vector<int64_t> decoded(count, 0);
        ASSERT_TRUE(codec.value().decode(bytes, 0, decoded).has_value());
        EXPECT_EQ(decoded, values);
        // The stream stays exactly as long as the values need.
        EXPECT_EQ(bytes.size(), (count * static_cast<size_t>(bits) + 7) / 8);
        for (size_t i = 0; i < count; ++i) {
            ASSERT_EQ(oracle_value_at(bytes, bits, i), values[i]) << "value " << i;
        }
    }
}

TEST(BulkOperationTest, StreamHelpersMatchTheScalarModel) {
    for (const int32_t bits : {1, 4, 8, 12, 17, 24, 32, 40, 64}) {
        SCOPED_TRACE(::testing::Message() << "bits=" << bits);
        const size_t count = 37;
        const std::vector<int64_t> values = make_values(count, bits, static_cast<uint32_t>(bits) + 3);
        const std::vector<uint8_t> expected = oracle_encode(values, bits);

        std::vector<uint8_t> bytes(expected.size() * 4 + 16, 0xAA);
        ByteArrayDataOutput out{std::span<uint8_t>(bytes)};
        ASSERT_TRUE(write_packed_stream(out, values, bits).has_value());
        ASSERT_EQ(out.get_position(), expected.size());
        EXPECT_EQ(std::vector<uint8_t>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(expected.size())),
                  expected);

        ByteArrayDataInput in{std::span<const uint8_t>(expected)};
        auto decoded = read_packed_stream(in, count, bits);
        ASSERT_TRUE(decoded.has_value()) << decoded.error().to_string();
        EXPECT_EQ(decoded.value(), values);
    }
}

TEST(BulkOperationTest, RejectsBuffersThatAreTooSmall) {
    auto codec = bulk_operation(21);
    ASSERT_TRUE(codec.has_value());
    const size_t values_per_group = codec.value().operation().byte_value_count();
    const std::vector<int64_t> values = make_values(values_per_group, 21, 5);

    std::vector<uint8_t> too_small((values_per_group * 21) / 8 - 1, 0);
    auto failed = codec.value().encode(values, 0, too_small);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().kind(), ErrorKind::kIllegalArgument);

    std::vector<int64_t> decoded(values_per_group, 0);
    std::vector<uint8_t> truncated(1, 0);
    auto failed_decode = codec.value().decode(truncated, 0, decoded);
    ASSERT_FALSE(failed_decode.has_value());
    EXPECT_EQ(failed_decode.error().kind(), ErrorKind::kEof);
}

TEST(BulkOperationTest, ComputeIterationsRespectsTheBudgetAndTheReaderSize) {
    auto codec = bulk_operation(12);
    ASSERT_TRUE(codec.has_value());
    const BulkOperation &operation = codec.value().operation();
    const auto bytes_per_iteration =
            static_cast<int32_t>(operation.byte_block_count()) + 8 * static_cast<int32_t>(operation.byte_value_count());

    // A budget that fits one iteration is clamped to one.
    EXPECT_EQ(operation.compute_iterations(1000, bytes_per_iteration / 2), 1);
    // A generous budget with a small reader allocates only for the reader.
    EXPECT_EQ(operation.compute_iterations(static_cast<int32_t>(operation.byte_value_count()), 1 << 20), 1);
    // Otherwise the budget decides.
    const int32_t expected = (1 << 20) / bytes_per_iteration;
    EXPECT_GE(operation.compute_iterations(1 << 20, 1 << 20), expected);
}

TEST(BulkOperationTest, SingleBlockLayoutRoundTrips) {
    for (const int32_t bits : {1, 2, 4, 8, 9, 16, 21, 32}) {
        SCOPED_TRACE(::testing::Message() << "bits=" << bits);
        auto operation = bulk_operation_of(PackedFormat::kPackedSingleBlock, bits);
        ASSERT_TRUE(operation.has_value());
        ASSERT_EQ((*operation.value()).byte_block_count(), 8U);
        const size_t values_per_block = (*operation.value()).byte_value_count();
        ASSERT_EQ(values_per_block, static_cast<size_t>(64 / bits));

        const size_t blocks = 3;
        const std::vector<int64_t> values = make_values(values_per_block * blocks, bits, static_cast<uint32_t>(bits) + 11);

        std::vector<uint8_t> bytes(8 * blocks, 0);
        (*operation.value()).encode_i64_to_u8(values, 0, bytes, 0, blocks);
        std::vector<int64_t> decoded(values.size(), 0);
        (*operation.value()).decode_u8_to_i64(bytes, 0, decoded, 0, blocks);
        EXPECT_EQ(decoded, values);
    }

    // Widths that do not divide 64 are rejected.
    auto unsupported = bulk_operation_of(PackedFormat::kPackedSingleBlock, 11);
    ASSERT_FALSE(unsupported.has_value());
    EXPECT_EQ(unsupported.error().kind(), ErrorKind::kIllegalArgument);
    auto out_of_range = bulk_operation_of(PackedFormat::kPackedSingleBlock, 33);
    ASSERT_FALSE(out_of_range.has_value());
    EXPECT_EQ(out_of_range.error().kind(), ErrorKind::kArrayIndexOutOfBounds);
}

TEST(BulkOperationTest, WriteLongIsBigEndian) {
    std::vector<uint8_t> bytes(9, 0);
    const size_t next = BulkOperation::write_long(0x0102030405060708ULL, bytes, 1);
    EXPECT_EQ(next, 9U);
    EXPECT_EQ(bytes[0], 0);
    for (size_t i = 0; i < 8; ++i) {
        EXPECT_EQ(bytes[1 + i], static_cast<uint8_t>(i + 1)) << "byte " << i;
    }
}
