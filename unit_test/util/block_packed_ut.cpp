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

#include <cstdint>
#include <span>
#include <string>
#include <vector>

module lucene_core:ut.block_packed;

import lucene_core;

using namespace lucene;

namespace {

constexpr size_t BLOCK = 64;

/// Writes `values` with one `BlockPackedWriter` and returns the bytes.
std::vector<uint8_t> write_packed(const std::vector<int64_t> &values, size_t block_size = BLOCK) {
    ByteArrayOutput out("packed");
    auto writer = BlockPackedWriter::create(block_size);
    EXPECT_TRUE(writer.has_value());
    for (const int64_t value : values) {
        EXPECT_TRUE(writer.value().add(value, out).has_value());
    }
    EXPECT_TRUE(writer.value().finish(out).has_value());
    return std::vector<uint8_t>(out.bytes().begin(), out.bytes().end());
}

} // namespace

TEST(BlockPackedTest, RoundTripsValuesAcrossBlocks) {
    std::vector<int64_t> values;
    for (int32_t i = 0; i < 200; ++i) {
        values.push_back(static_cast<int64_t>(i) * i - 500);
    }
    const auto bytes = write_packed(values);
    ASSERT_FALSE(bytes.empty());

    ByteArrayDataInput in(bytes);
    auto reader = BlockPackedReaderIterator::create(PackedInts::VERSION_CURRENT, BLOCK, values.size());
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    for (size_t i = 0; i < values.size(); ++i) {
        auto value = reader.value().next_value(in);
        ASSERT_TRUE(value.has_value()) << (value ? "" : value.error().to_string());
        EXPECT_EQ(value.value(), values[i]) << "at index " << i;
    }
    EXPECT_EQ(reader.value().ord(), values.size());
    // The stream must not be over-read.
    EXPECT_EQ(in.eof(), true);
}

TEST(BlockPackedTest, ReadsBatches) {
    std::vector<int64_t> values;
    for (int32_t i = 0; i < 150; ++i) {
        values.push_back(1000 - static_cast<int64_t>(i) * 3);
    }
    const auto bytes = write_packed(values);

    ByteArrayDataInput in(bytes);
    auto reader = BlockPackedReaderIterator::create(PackedInts::VERSION_CURRENT, BLOCK, values.size());
    ASSERT_TRUE(reader.has_value());

    size_t index = 0;
    size_t batch = 1;
    while (index < values.size()) {
        auto chunk = reader.value().next_batch(batch, in);
        ASSERT_TRUE(chunk.has_value()) << (chunk ? "" : chunk.error().to_string());
        ASSERT_FALSE(chunk.value().empty());
        for (const int64_t value : chunk.value()) {
            ASSERT_LT(index, values.size());
            EXPECT_EQ(value, values[index]) << "at index " << index;
            ++index;
        }
        batch = batch * 3 + 1;
    }
    EXPECT_EQ(index, values.size());
    EXPECT_EQ(reader.value().next_value(in).has_value(), false);
}

TEST(BlockPackedTest, SkipsWithinAndAcrossBlocks) {
    std::vector<int64_t> values;
    for (int32_t i = 0; i < 300; ++i) {
        values.push_back(static_cast<int64_t>(i) * 7);
    }
    const auto bytes = write_packed(values);

    ByteArrayDataInput in(bytes);
    auto reader = BlockPackedReaderIterator::create(PackedInts::VERSION_CURRENT, BLOCK, values.size());
    ASSERT_TRUE(reader.has_value());

    // Inside the first block.
    ASSERT_TRUE(reader.value().skip(5, in).has_value());
    EXPECT_EQ(reader.value().next_value(in).value(), values[5]);
    EXPECT_EQ(reader.value().ord(), 6);

    // Across several whole blocks (one value buffered, then 2 * 64 + 3).
    ASSERT_TRUE(reader.value().skip(2 * BLOCK + 3, in).has_value());
    EXPECT_EQ(reader.value().ord(), 6 + 2 * BLOCK + 3);
    EXPECT_EQ(reader.value().next_value(in).value(), values[6 + 2 * BLOCK + 3]);

    // To the very end.
    ASSERT_TRUE(reader.value().skip(values.size() - reader.value().ord() - 1, in).has_value());
    EXPECT_EQ(reader.value().next_value(in).value(), values.back());
    EXPECT_FALSE(reader.value().next_value(in).has_value());

    // Skipping past the end is an error.
    auto too_far = BlockPackedReaderIterator::create(PackedInts::VERSION_CURRENT, BLOCK, 10);
    ASSERT_TRUE(too_far.has_value());
    ByteArrayDataInput other(bytes);
    EXPECT_FALSE(too_far.value().skip(11, other).has_value());
}

TEST(BlockPackedTest, ConstantBlocksNeedNoValues) {
    // All values equal and non-zero: the block is a token plus the min value.
    std::vector<int64_t> values(100, int64_t{42});
    const auto bytes = write_packed(values);
    // 2 blocks, each one byte token plus one zig-zag vlong of (42 - 1).
    EXPECT_LE(bytes.size(), size_t{8});

    ByteArrayDataInput in(bytes);
    auto reader = BlockPackedReaderIterator::create(PackedInts::VERSION_CURRENT, BLOCK, values.size());
    ASSERT_TRUE(reader.has_value());
    for (size_t i = 0; i < values.size(); ++i) {
        auto value = reader.value().next_value(in);
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(value.value(), int64_t{42});
    }

    // A constant zero block is a single token byte per block.
    const auto zeros = write_packed(std::vector<int64_t>(BLOCK, 0));
    EXPECT_EQ(zeros.size(), size_t{1});
}

TEST(BlockPackedTest, HandlesNegativeValues) {
    std::vector<int64_t> values;
    for (int32_t i = 0; i < 70; ++i) {
        values.push_back(-1000000 - static_cast<int64_t>(i));
    }
    values.push_back(std::numeric_limits<int64_t>::min() / 2);
    const auto bytes = write_packed(values);

    ByteArrayDataInput in(bytes);
    auto reader = BlockPackedReaderIterator::create(PackedInts::VERSION_CURRENT, BLOCK, values.size());
    ASSERT_TRUE(reader.has_value());
    for (size_t i = 0; i < values.size(); ++i) {
        auto value = reader.value().next_value(in);
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(value.value(), values[i]) << "at index " << i;
    }
}

TEST(BlockPackedTest, ResetStartsANewSequence) {
    std::vector<int64_t> first;
    for (int32_t i = 0; i < 100; ++i) {
        first.push_back(i);
    }
    std::vector<int64_t> second;
    for (int32_t i = 0; i < 70; ++i) {
        second.push_back(5000 + static_cast<int64_t>(i) * 11);
    }

    ByteArrayOutput out("packed");
    auto writer = BlockPackedWriter::create(BLOCK);
    ASSERT_TRUE(writer.has_value());
    for (const int64_t value : first) {
        ASSERT_TRUE(writer.value().add(value, out).has_value());
    }
    ASSERT_TRUE(writer.value().finish(out).has_value());
    writer.value().reset();
    for (const int64_t value : second) {
        ASSERT_TRUE(writer.value().add(value, out).has_value());
    }
    ASSERT_TRUE(writer.value().finish(out).has_value());
    EXPECT_EQ(writer.value().ord(), second.size());

    std::vector<uint8_t> bytes(out.bytes().begin(), out.bytes().end());
    ByteArrayDataInput in(bytes);
    auto reader = BlockPackedReaderIterator::create(PackedInts::VERSION_CURRENT, BLOCK, first.size());
    ASSERT_TRUE(reader.has_value());
    for (const int64_t value : first) {
        EXPECT_EQ(reader.value().next_value(in).value(), value);
    }

    // The second sequence starts right where the first one ended.
    reader.value().reset(second.size());
    for (const int64_t value : second) {
        EXPECT_EQ(reader.value().next_value(in).value(), value);
    }
    EXPECT_TRUE(in.eof());
}

TEST(BlockPackedTest, WritesPackedBitStreamsAndDirectValues) {
    // Format.PACKED: big endian, MSB first, exactly ceil(count * bits / 8) bytes.
    const std::vector<int64_t> values = {0, 1, 3, 7, 15, 1023, 5};
    ByteArrayOutput out("packed-stream");
    ASSERT_TRUE(write_packed_stream(out, std::span<const int64_t>(values), 10).has_value());
    EXPECT_EQ(out.bytes().size(), packed_stream_byte_count(values.size(), 10));
    EXPECT_EQ(out.bytes().size(), size_t{9});  // 7 * 10 bits = 70 bits

    std::vector<uint8_t> bytes(out.bytes().begin(), out.bytes().end());
    ByteArrayDataInput in(bytes);
    auto decoded = read_packed_stream(in, values.size(), 10);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded.value().size(), values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        EXPECT_EQ(decoded.value()[i], values[i]) << "at index " << i;
    }
    EXPECT_TRUE(in.eof());

    // A zero-width stream occupies no bytes at all.
    ByteArrayOutput empty("empty");
    ASSERT_TRUE(write_packed_stream(empty, std::span<const int64_t>(values), 0).has_value());
    EXPECT_TRUE(empty.bytes().empty());
    EXPECT_EQ(packed_stream_byte_count(values.size(), 0), size_t{0});

    // DirectWriter output is the opposite layout: little endian, LSB first.  It
    // only accepts the widths `bits_required` returns.
    auto direct_bits = DirectWriter::bits_required(1023);
    ASSERT_TRUE(direct_bits.has_value()) << (direct_bits ? "" : direct_bits.error().to_string());
    EXPECT_EQ(direct_bits.value(), 12);  // the next supported width above 10
    ByteArrayOutput direct_out("direct");
    auto writer = DirectWriter::get_instance(direct_out, static_cast<int64_t>(values.size()), direct_bits.value());
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    for (const int64_t value : values) {
        ASSERT_TRUE(writer.value().add(value).has_value());
    }
    ASSERT_TRUE(writer.value().finish().has_value());

    const std::span<const uint8_t> direct_bytes = direct_out.bytes();
    for (size_t i = 0; i < values.size(); ++i) {
        auto value = direct_packed_get(direct_bytes, direct_bits.value(), i);
        ASSERT_TRUE(value.has_value()) << (value ? "" : value.error().to_string());
        EXPECT_EQ(value.value(), values[i]) << "at index " << i;
    }
    // Reading past a buffer that is too short fails instead of returning garbage.
    std::vector<uint8_t> short_bytes(2, 0);
    EXPECT_FALSE(direct_packed_get(std::span<const uint8_t>(short_bytes), 32, 1).has_value());
}

TEST(BlockPackedTest, RejectsInvalidArguments) {
    ByteArrayOutput out("bad");
    EXPECT_FALSE(BlockPackedWriter::create(63).has_value());
    EXPECT_FALSE(BlockPackedWriter::create(static_cast<size_t>(BlockPackedWriter::MAX_BLOCK_SIZE) + 64).has_value());

    auto writer = BlockPackedWriter::create(BLOCK);
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer.value().add(1, out).has_value());
    ASSERT_TRUE(writer.value().finish(out).has_value());
    // No value may be added after finish.
    EXPECT_FALSE(writer.value().add(2, out).has_value());
    EXPECT_FALSE(writer.value().finish(out).has_value());

    EXPECT_FALSE(BlockPackedReaderIterator::create(PackedInts::VERSION_CURRENT, 0, 1).has_value());

    std::vector<uint8_t> bytes(out.bytes().begin(), out.bytes().end());
    ByteArrayDataInput in(bytes);
    auto reader = BlockPackedReaderIterator::create(PackedInts::VERSION_CURRENT, BLOCK, 1);
    ASSERT_TRUE(reader.has_value());
    EXPECT_FALSE(reader.value().next_batch(0, in).has_value());
    EXPECT_FALSE(BlockPackedReaderIterator::create(PackedInts::VERSION_CURRENT, BLOCK, 0)
                         .value()
                         .next_value(in)
                         .has_value());

    std::vector<uint8_t> empty_bytes;
    ByteArrayDataInput empty_in(empty_bytes);
    auto empty_reader = BlockPackedReaderIterator::create(PackedInts::VERSION_CURRENT, BLOCK, 5);
    ASSERT_TRUE(empty_reader.has_value());
    EXPECT_FALSE(empty_reader.value().next_value(empty_in).has_value());
    EXPECT_FALSE(read_packed_stream(empty_in, 5, 8).has_value());
    EXPECT_FALSE(write_packed_stream(out, std::span<const int64_t>(), -1).has_value());
    EXPECT_FALSE(direct_packed_get(std::span<const uint8_t>(empty_bytes), 0, 0).has_value());
}
