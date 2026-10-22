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
#include <limits>

module lucene_core:ut.fst_storage;

import lucene_core;

using namespace lucene;

namespace {

/// Counts how many blocks were handed out and recycled.
class CountingAllocator final : public ByteBlockAllocator {
public:
    [[nodiscard]] std::vector<uint8_t> get_byte_block() override {
        ++allocated;
        return std::vector<uint8_t>(static_cast<size_t>(BYTE_BLOCK_SIZE), 0);
    }

    void recycle_byte_blocks(std::span<std::vector<uint8_t>> blocks) override { recycled += blocks.size(); }

    size_t allocated{0};
    size_t recycled{0};
};

} // namespace

TEST(ByteBlockPoolTest, AppendsAcrossBlockBoundaries) {
    ByteBlockPool pool;
    // Cross one block boundary and end inside the second block.
    const size_t length = static_cast<size_t>(BYTE_BLOCK_SIZE) + 1234;
    std::vector<uint8_t> data(length);
    for (size_t i = 0; i < length; ++i) {
        data[i] = static_cast<uint8_t>(i % 251);
    }

    ASSERT_TRUE(pool.append(data).has_value());
    EXPECT_EQ(pool.get_bytes_used(), static_cast<int64_t>(length));
    EXPECT_EQ(pool.buffer_count(), 2u);
    ASSERT_TRUE(pool.current_buffer().has_value());
    EXPECT_EQ(pool.current_buffer().value(), 1u);

    std::vector<uint8_t> restored(length);
    ASSERT_TRUE(pool.read_bytes(0, restored, 0, length).has_value());
    EXPECT_EQ(restored, data);

    // A read that starts near the end of a block must stitch two blocks.
    const size_t start = static_cast<size_t>(BYTE_BLOCK_SIZE) - 10;
    std::vector<uint8_t> crossing(32);
    ASSERT_TRUE(pool.read_bytes(static_cast<int64_t>(start), crossing, 0, crossing.size()).has_value());
    for (size_t i = 0; i < crossing.size(); ++i) {
        EXPECT_EQ(crossing[i], data[start + i]) << "index " << i;
    }

    // Single-byte access and the owned bytes-ref accessor agree with the input.
    EXPECT_EQ(pool.read_byte(static_cast<size_t>(BYTE_BLOCK_SIZE) + 5),
              data[static_cast<size_t>(BYTE_BLOCK_SIZE) + 5]);
    auto ref = pool.bytes_ref(static_cast<int64_t>(BYTE_BLOCK_SIZE) - 4, 8);
    ASSERT_TRUE(ref.has_value());
    for (size_t i = 0; i < 8; ++i) {
        EXPECT_EQ(ref->as_slice()[i], data[static_cast<size_t>(BYTE_BLOCK_SIZE) - 4 + i]);
    }

    // Reading from a block that was never allocated fails instead of touching
    // unallocated memory.
    EXPECT_FALSE(pool.read_bytes(static_cast<int64_t>(2) * BYTE_BLOCK_SIZE, crossing, 0, crossing.size())
                     .has_value());
}

TEST(ByteBlockPoolTest, ResetCanReuseTheFirstBlock) {
    CountingAllocator allocator;
    ByteBlockPool pool(allocator);
    const std::vector<uint8_t> payload(static_cast<size_t>(BYTE_BLOCK_SIZE) + 10, 7);
    ASSERT_TRUE(pool.append(payload).has_value());
    EXPECT_EQ(allocator.allocated, 2u);
    EXPECT_EQ(pool.buffer_count(), 2u);

    pool.reset(false, true);
    EXPECT_EQ(allocator.recycled, 1u);
    EXPECT_EQ(pool.buffer_count(), 1u);
    ASSERT_TRUE(pool.current_buffer().has_value());
    EXPECT_EQ(pool.current_buffer().value(), 0u);
    EXPECT_EQ(pool.get_bytes_used(), 0);

    // Reuse must not reallocate the kept block.
    ASSERT_TRUE(pool.append(std::span<const uint8_t>(payload.data(), 128)).has_value());
    EXPECT_EQ(allocator.allocated, 2u);
    EXPECT_EQ(pool.get_bytes_used(), 128);
}

TEST(ByteBlockPoolTest, AppendFromAnotherPool) {
    ByteBlockPool source;
    const std::vector<uint8_t> payload(static_cast<size_t>(BYTE_BLOCK_SIZE) + 300, 3);
    ASSERT_TRUE(source.append(payload).has_value());

    EXPECT_EQ(source.read_byte(static_cast<size_t>(BYTE_BLOCK_SIZE) - 50), 3);
    EXPECT_EQ(source.read_byte(static_cast<size_t>(BYTE_BLOCK_SIZE) + 5), 3);

    ByteBlockPool target;
    ASSERT_TRUE(target.append_from(source, static_cast<int64_t>(BYTE_BLOCK_SIZE) - 50, 200).has_value());
    EXPECT_EQ(target.get_bytes_used(), 200);
    EXPECT_EQ(target.read_byte(0), 3);
    EXPECT_EQ(target.buffer_count(), 1u);
    EXPECT_EQ(target.buffer(0).size(), static_cast<size_t>(BYTE_BLOCK_SIZE));
    EXPECT_EQ(target.read_byte(49), 3);
    EXPECT_EQ(target.read_byte(50), 3);
    EXPECT_EQ(target.read_byte(199), 3);
    {
        std::vector<uint8_t> probe(8);
        ASSERT_TRUE(source.read_bytes(BYTE_BLOCK_SIZE, probe, 0, probe.size()).has_value());
        for (const uint8_t value : probe) {
            EXPECT_EQ(value, 3);
        }
    }

    std::vector<uint8_t> restored(200);
    ASSERT_TRUE(target.read_bytes(0, restored, 0, restored.size()).has_value());
    for (const uint8_t value : restored) {
        EXPECT_EQ(value, 3);
    }
}

TEST(ReverseBytesReaderTest, ReadsBackwards) {
    const std::vector<uint8_t> bytes{'a', 'b', 'c', 'd', 'e'};
    ReverseBytesReader reader(bytes);
    EXPECT_EQ(reader.get_position(), 0);

    // Start at the last byte, like the FST reader does after deserializing a node.
    reader.set_position(static_cast<int64_t>(bytes.size()) - 1);
    auto last = reader.read_byte();
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last.value(), 'e');
    EXPECT_EQ(reader.get_position(), 3);

    std::vector<uint8_t> two(2);
    ASSERT_TRUE(reader.read_bytes(two).has_value());
    EXPECT_EQ(two, (std::vector<uint8_t>{'d', 'c'}));

    ASSERT_TRUE(reader.skip_bytes(1).has_value());
    EXPECT_EQ(reader.get_position(), 0);
    auto first = reader.read_byte();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first.value(), 'a');

    // Reading past the beginning is an error, not undefined behaviour.
    EXPECT_FALSE(reader.read_byte().has_value());
}

TEST(ReverseBytesReaderTest, MultiByteGetsAreReversed) {
    const std::vector<uint8_t> bytes{1, 2, 3, 4};
    ReverseBytesReader reader(bytes);
    reader.set_position(3);
    auto all = reader.get_bytes(4);
    ASSERT_TRUE(all.has_value()) << (all ? "" : all.error().to_string());
    EXPECT_EQ(all.value(), (std::vector<uint8_t>{4, 3, 2, 1}));
}

TEST(ReadWriteDataOutputTest, ServesBothDirections) {
    ReadWriteDataOutput output;
    const std::string text = "abcdef";
    ASSERT_TRUE(output.write_bytes(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()),
                                                           text.size()))
                     .has_value());
    EXPECT_EQ(output.size(), text.size());

    // Reading is only allowed once the buffer is frozen.
    auto too_early = output.get_reverse_bytes_reader();
    ASSERT_FALSE(too_early.has_value());
    EXPECT_EQ(too_early.error().kind(), ErrorKind::kIllegalState);

    ASSERT_TRUE(output.freeze().has_value());
    auto reader = output.get_reverse_bytes_reader();
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    reader->set_position(static_cast<int64_t>(text.size()) - 1);

    std::string reversed;
    for (size_t i = 0; i < text.size(); ++i) {
        auto byte = reader->read_byte();
        ASSERT_TRUE(byte.has_value());
        reversed.push_back(static_cast<char>(byte.value()));
    }
    EXPECT_EQ(reversed, "fedcba");

    // The same bytes can also be copied forward into a real output.
    std::vector<uint8_t> buffer(16, 0);
    ByteArrayDataOutput out(buffer);
    ASSERT_TRUE(output.write_to(out).has_value());
    EXPECT_EQ(out.get_position(), text.size());
    EXPECT_EQ(std::string(buffer.begin(), buffer.begin() + static_cast<ptrdiff_t>(text.size())), text);

    output.reset();
    EXPECT_EQ(output.size(), 0u);
    EXPECT_FALSE(output.frozen());
}
