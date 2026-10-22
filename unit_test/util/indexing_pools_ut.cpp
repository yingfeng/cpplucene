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
#include <expected>
#include <memory>
#include <string>
#include <vector>

module lucene_core:ut.indexing_pools;

import lucene_core;

using namespace lucene;

namespace {

/// Appends one byte to the slice chain, exactly like
/// `TermsHashPerField::write_byte`: a non-zero byte already in the slot marks the
/// end of the slice, so a new one is allocated first.
class SliceWriter {
public:
    SliceWriter(ByteSlicePool &slices, ByteBlockPool &bytes) : slices_(slices), bytes_(bytes) {}

    /// Allocates the first slice and returns the flat start offset.
    Result<int32_t> start() {
        auto created = slices_.new_slice(ByteSlicePool::FIRST_LEVEL_SIZE, bytes_);
        if (!created.has_value()) {
            return created;
        }
        address_ = created.value();
        return address_;
    }

    [[nodiscard]] VoidResult write_byte(uint8_t b) {
        auto slot = locate();
        if (!slot.has_value()) {
            return std::unexpected(std::move(slot.error()));
        }
        auto [block_index, offset] = slot.value();
        if (bytes_.mutable_buffer(block_index)[offset] != 0) {
            // End of this slice: allocate the next one and continue there.
            auto next = slices_.alloc_slice(block_index, offset, bytes_);
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
            address_ = static_cast<int32_t>(next.value()) + bytes_.byte_offset();
            block_index = bytes_.current_buffer().value();
            offset = next.value();
        }
        bytes_.mutable_buffer(block_index)[offset] = b;
        ++address_;
        return {};
    }

    [[nodiscard]] int32_t address() const noexcept { return address_; }

private:
    [[nodiscard]] Result<std::pair<size_t, size_t>> locate() const {
        const auto block_index = static_cast<size_t>(address_) >> BYTE_BLOCK_SHIFT;
        const auto offset = static_cast<size_t>(address_) & BYTE_BLOCK_MASK;
        if (block_index >= bytes_.buffer_count()) {
            return err(LuceneError::illegal_state("slice address is outside the pool"));
        }
        return std::pair<size_t, size_t>{block_index, offset};
    }

    ByteSlicePool &slices_;
    ByteBlockPool &bytes_;
    int32_t address_{0};
};

BytesRef term(std::string_view text) { return BytesRef::from_string(text); }

std::string read_term(const BytesRefBlockPool &pool, const ByteBlockPool &blocks, int32_t start) {
    auto position = pool.fill_bytes_ref(start, blocks);
    auto value = pool.bytes_ref(position, blocks);
    return value.has_value() ? std::string(value.value().as_slice().begin(), value.value().as_slice().end())
                             : std::string("<error>");
}

} // namespace

// ---------------------------------------------------------------------------
// IntBlockPool
// ---------------------------------------------------------------------------

TEST(IndexingPoolsTest, IntBlockPoolAdvancesThroughBlocks) {
    IntBlockPool pool;
    EXPECT_FALSE(pool.buffer_upto().has_value());
    EXPECT_EQ(pool.int_upto(), INT_BLOCK_SIZE);
    EXPECT_EQ(pool.int_offset(), -INT_BLOCK_SIZE);

    ASSERT_TRUE(pool.next_buffer().has_value());
    EXPECT_EQ(pool.buffer_upto().value(), 0U);
    EXPECT_EQ(pool.int_upto(), 0);
    EXPECT_EQ(pool.int_offset(), 0);
    EXPECT_EQ(pool.buffer(0).size(), static_cast<size_t>(INT_BLOCK_SIZE));

    pool.set_int_upto(42);
    EXPECT_EQ(pool.int_upto(), 42);
    pool.buffer_mut(0)[41] = 7;
    EXPECT_EQ(pool.buffer(0)[41], 7);

    ASSERT_TRUE(pool.next_buffer().has_value());
    EXPECT_EQ(pool.buffer_upto().value(), 1U);
    EXPECT_EQ(pool.int_upto(), 0);
    EXPECT_EQ(pool.int_offset(), INT_BLOCK_SIZE);
    EXPECT_EQ(pool.buffers().size(), 2U);
}

TEST(IndexingPoolsTest, IntBlockPoolResetKeepsOrDropsTheFirstBlock) {
    IntBlockPool pool;
    ASSERT_TRUE(pool.next_buffer().has_value());
    pool.buffer_mut(0)[0] = 5;
    pool.set_int_upto(1);
    ASSERT_TRUE(pool.next_buffer().has_value());
    pool.buffer_mut(1)[0] = 9;

    // keep the head block, zero fill what is reused
    pool.reset(true, true);
    EXPECT_EQ(pool.buffer_upto().value(), 0U);
    EXPECT_EQ(pool.int_upto(), 0);
    EXPECT_EQ(pool.int_offset(), 0);
    EXPECT_EQ(pool.buffers().size(), 1U);
    EXPECT_EQ(pool.buffer(0)[0], 0);

    // drop everything
    pool.reset(false, false);
    EXPECT_FALSE(pool.buffer_upto().has_value());
    EXPECT_EQ(pool.int_offset(), -INT_BLOCK_SIZE);
    EXPECT_TRUE(pool.buffers().empty());
}

// ---------------------------------------------------------------------------
// BytesRefBlockPool
// ---------------------------------------------------------------------------

TEST(IndexingPoolsTest, BytesRefBlockPoolRoundTripsTerms) {
    ByteBlockPool blocks;
    BytesRefBlockPool pool;

    const auto short_term = term("hello");
    auto short_start = pool.add_bytes_ref(short_term, blocks);
    ASSERT_TRUE(short_start.has_value());
    EXPECT_EQ(read_term(pool, blocks, short_start.value()), "hello");
    EXPECT_TRUE(pool.equals(short_start.value(), short_term, blocks));
    EXPECT_FALSE(pool.equals(short_start.value(), term("hellp"), blocks));

    // A 2-byte length prefix is used from 128 bytes on.
    const std::string long_text(200, 'x');
    auto long_start = pool.add_bytes_ref(term(long_text), blocks);
    ASSERT_TRUE(long_start.has_value());
    EXPECT_NE(long_start.value(), short_start.value());
    EXPECT_EQ(read_term(pool, blocks, long_start.value()), long_text);
    EXPECT_TRUE(pool.equals(long_start.value(), term(long_text), blocks));

    // The hash has to agree with BytesRef::hash_code, the terms hash relies on it.
    EXPECT_EQ(pool.hash(short_start.value(), blocks), short_term.hash_code());
    EXPECT_EQ(pool.hash(long_start.value(), blocks), term(long_text).hash_code());
    // ... including the single byte lookup table.
    auto one = pool.add_bytes_ref(term("a"), blocks);
    ASSERT_TRUE(one.has_value());
    EXPECT_EQ(pool.hash(one.value(), blocks), term("a").hash_code());
}

TEST(IndexingPoolsTest, BytesRefBlockPoolRejectsOversizedTerms) {
    ByteBlockPool blocks;
    BytesRefBlockPool pool;
    const std::string too_long(BYTE_BLOCK_SIZE, 'a');
    auto added = pool.add_bytes_ref(term(too_long), blocks);
    ASSERT_FALSE(added.has_value());
    EXPECT_EQ(added.error().kind(), ErrorKind::kMaxBytesLengthExceeded);
}

TEST(IndexingPoolsTest, BytesRefBlockPoolMovesToANewBlockWhenFull) {
    ByteBlockPool blocks;
    BytesRefBlockPool pool;

    std::vector<int32_t> starts;
    // 1000 terms of 60 bytes fill several 32kB blocks.
    for (int32_t i = 0; i < 1000; ++i) {
        const std::string text = "term-" + std::to_string(i) + std::string(50, 'x');
        auto added = pool.add_bytes_ref(term(text), blocks);
        ASSERT_TRUE(added.has_value()) << added.error().to_string();
        starts.push_back(added.value());
    }
    EXPECT_GT(blocks.buffer_count(), 1U);
    for (int32_t i = 0; i < 1000; ++i) {
        const std::string text = "term-" + std::to_string(i) + std::string(50, 'x');
        EXPECT_EQ(read_term(pool, blocks, starts[static_cast<size_t>(i)]), text);
    }
}

// ---------------------------------------------------------------------------
// ByteSlicePool + ByteSliceReader
// ---------------------------------------------------------------------------

TEST(IndexingPoolsTest, ByteSlicePoolRejectsSlicesLargerThanABlock) {
    ByteBlockPool blocks;
    ByteSlicePool slices;
    auto created = slices.new_slice(BYTE_BLOCK_SIZE + 1, blocks);
    ASSERT_FALSE(created.has_value());
    EXPECT_EQ(created.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(IndexingPoolsTest, ByteSliceReaderReadsAChainOfSlices) {
    ByteBlockPool blocks;
    ByteSlicePool slices;
    SliceWriter writer(slices, blocks);

    auto start = writer.start();
    ASSERT_TRUE(start.has_value()) << start.error().to_string();

    // Enough bytes to walk through several slice levels (5, 14, 20, 30, ...).
    std::vector<uint8_t> expected;
    for (size_t i = 0; i < 500; ++i) {
        // Skip zero so that the "non-zero means end of slice" rule holds.
        const auto byte = static_cast<uint8_t>((i % 255) + 1);
        expected.push_back(byte);
        auto written = writer.write_byte(byte);
        ASSERT_TRUE(written.has_value()) << written.error().to_string();
    }

    ByteSliceReader reader(blocks);
    reader.init(static_cast<size_t>(start.value()), static_cast<size_t>(writer.address()));
    EXPECT_FALSE(reader.eof());

    std::vector<uint8_t> actual;
    while (!reader.eof()) {
        auto byte = reader.read_byte();
        ASSERT_TRUE(byte.has_value()) << byte.error().to_string();
        actual.push_back(byte.value());
    }
    EXPECT_EQ(actual, expected);
}

TEST(IndexingPoolsTest, ByteSliceReaderReadsAndSkipsInBulk) {
    ByteBlockPool blocks;
    ByteSlicePool slices;
    SliceWriter writer(slices, blocks);

    auto start = writer.start();
    ASSERT_TRUE(start.has_value()) << start.error().to_string();
    const std::string text = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (const char ch : text) {
        ASSERT_TRUE(writer.write_byte(static_cast<uint8_t>(ch)).has_value());
    }

    ByteSliceReader reader(blocks);
    reader.init(static_cast<size_t>(start.value()), static_cast<size_t>(writer.address()));

    std::vector<uint8_t> head(10);
    ASSERT_TRUE(reader.read_bytes(head).has_value());
    EXPECT_EQ(std::string(head.begin(), head.end()), "abcdefghij");

    ASSERT_TRUE(reader.skip_bytes(10).has_value());
    std::vector<uint8_t> tail(16);
    ASSERT_TRUE(reader.read_bytes(tail).has_value());
    EXPECT_EQ(std::string(tail.begin(), tail.end()), "uvwxyz0123456789");
    EXPECT_TRUE(reader.eof());

    EXPECT_FALSE(reader.skip_bytes(-1).has_value());
}

TEST(IndexingPoolsTest, ByteSliceReaderCopiesTheChainToAnOutput) {
    ByteBlockPool blocks;
    ByteSlicePool slices;
    SliceWriter writer(slices, blocks);

    auto start = writer.start();
    ASSERT_TRUE(start.has_value()) << start.error().to_string();
    std::vector<uint8_t> expected;
    for (size_t i = 0; i < 60; ++i) {
        const auto byte = static_cast<uint8_t>((i % 200) + 2);
        expected.push_back(byte);
        ASSERT_TRUE(writer.write_byte(byte).has_value());
    }

    ByteSliceReader reader(blocks);
    reader.init(static_cast<size_t>(start.value()), static_cast<size_t>(writer.address()));
    auto out = std::make_unique<GrowableByteArrayDataOutput>();
    EXPECT_EQ(reader.write(*out), expected.size());
    EXPECT_EQ(out->bytes().size(), expected.size());
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), out->bytes().begin()));
}
