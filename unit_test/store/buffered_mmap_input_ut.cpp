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

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

module lucene_core:ut.buffered_mmap_input;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view FILE_NAME = "_0.fdt";

/// 3000 pseudo random-ish bytes so that reads cross all buffer sizes used here.
std::vector<uint8_t> make_payload(size_t size) {
    std::vector<uint8_t> payload(size);
    for (size_t i = 0; i < size; ++i) {
        payload[i] = static_cast<uint8_t>((i * 37U + 11U) & 0xFFU);
    }
    return payload;
}

/// Writes `payload` as FILE_NAME into `directory`.
void write_memory_file(ByteBuffersDirectory &directory, std::span<const uint8_t> payload) {
    auto output = directory.create_output(FILE_NAME, io_context_default());
    ASSERT_TRUE(output.has_value());
    ASSERT_TRUE(output.value()->write_bytes(payload).has_value());
    ASSERT_TRUE(output.value()->close().has_value());
}

[[nodiscard]] Result<std::unique_ptr<BufferedIndexInput>> open_buffered_file(ByteBuffersDirectory &directory,
                                                                             size_t buffer_size) {
    auto input = directory.open_input(FILE_NAME, io_context_default());
    if (!input) {
        return std::unexpected(std::move(input.error()));
    }
    return open_buffered_with_buffer_size(std::move(input.value()), std::string(FILE_NAME), buffer_size);
}

// ---------------- BufferedIndexInput ----------------

TEST(BufferedIndexInputTest, ReadsSequentiallyAcrossBufferBoundaries) {
    const auto payload = make_payload(3000);
    ByteBuffersDirectory directory;
    write_memory_file(directory, payload);

    for (const size_t buffer_size : {size_t{8}, size_t{16}, size_t{1024}, size_t{4096}}) {
        auto input = open_buffered_file(directory, buffer_size);
        ASSERT_TRUE(input.has_value()) << buffer_size;
        auto &buffered = *input.value();

        EXPECT_EQ(buffered.buffer_size(), buffer_size);
        const auto length = buffered.length();
        ASSERT_TRUE(length.has_value());
        EXPECT_EQ(length.value(), payload.size());

        for (size_t i = 0; i < payload.size(); ++i) {
            auto byte = buffered.read_byte();
            ASSERT_TRUE(byte.has_value()) << "buffer_size=" << buffer_size << " pos=" << i;
            ASSERT_EQ(byte.value(), payload[i]) << "buffer_size=" << buffer_size << " pos=" << i;
        }
        EXPECT_EQ(buffered.get_file_pointer().value(), payload.size());
        // One byte past the end reports EOF.
        EXPECT_FALSE(buffered.read_byte().has_value());
    }
}

TEST(BufferedIndexInputTest, ReadsScalarsAndArrays) {
    const auto payload = make_payload(2050);
    ByteBuffersDirectory directory;
    write_memory_file(directory, payload);
    auto input = open_buffered_file(directory, 16);
    ASSERT_TRUE(input.has_value());
    auto &buffered = *input.value();

    auto first = buffered.read_int();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first.value(), BitUtil::get_i32_le(payload, 0));
    EXPECT_EQ(buffered.get_file_pointer().value(), 4U);

    auto second = buffered.read_long();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second.value(), BitUtil::get_i64_le(payload, 4));
    EXPECT_EQ(buffered.get_file_pointer().value(), 12U);

    auto short_value = buffered.read_short();
    ASSERT_TRUE(short_value.has_value());
    EXPECT_EQ(short_value.value(), BitUtil::get_i16_le(payload, 12));

    std::vector<uint8_t> tail(2000);
    ASSERT_TRUE(buffered.read_bytes(tail).has_value());
    EXPECT_EQ(buffered.get_file_pointer().value(), 14U + tail.size());
    EXPECT_TRUE(std::equal(tail.begin(), tail.end(), payload.begin() + 14));

    // A read starting inside the cached window still lands on the right bytes.
    ASSERT_TRUE(buffered.seek(1000).has_value());
    std::array<uint8_t, 40> mid{};
    ASSERT_TRUE(buffered.read_bytes(mid).has_value());
    EXPECT_TRUE(std::equal(mid.begin(), mid.end(), payload.begin() + 1000));
}

TEST(BufferedIndexInputTest, SeekBackwardsAndForwards) {
    const auto payload = make_payload(3000);
    ByteBuffersDirectory directory;
    write_memory_file(directory, payload);
    auto input = open_buffered_file(directory, 64);
    ASSERT_TRUE(input.has_value());
    auto &buffered = *input.value();

    ASSERT_TRUE(buffered.seek(2999).has_value());
    auto last = buffered.read_byte();
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last.value(), payload[2999]);

    ASSERT_TRUE(buffered.seek(10).has_value());
    auto tenth = buffered.read_byte();
    ASSERT_TRUE(tenth.has_value());
    EXPECT_EQ(tenth.value(), payload[10]);

    ASSERT_TRUE(buffered.seek(100).has_value());
    ASSERT_TRUE(buffered.skip_bytes(50).has_value());
    EXPECT_EQ(buffered.get_file_pointer().value(), 150U);
    auto skipped = buffered.read_byte();
    ASSERT_TRUE(skipped.has_value());
    EXPECT_EQ(skipped.value(), payload[150]);

    // Seeking exactly to the end is allowed, reading there is not.
    ASSERT_TRUE(buffered.seek(payload.size()).has_value());
    EXPECT_FALSE(buffered.read_byte().has_value());
    EXPECT_FALSE(buffered.seek(payload.size() + 1).has_value());
    EXPECT_FALSE(buffered.skip_bytes(1).has_value());
    EXPECT_FALSE(buffered.skip_bytes(-1).has_value());
}

TEST(BufferedIndexInputTest, SliceAndCloneAreIndependent) {
    const auto payload = make_payload(2500);
    ByteBuffersDirectory directory;
    write_memory_file(directory, payload);
    auto input = open_buffered_file(directory, 128);
    ASSERT_TRUE(input.has_value());

    auto slice = input.value()->slice("slice", 1000, 500);
    ASSERT_TRUE(slice.has_value());
    EXPECT_EQ(slice.value()->length().value(), 500U);
    auto clone = slice.value()->clone();
    ASSERT_TRUE(clone.has_value());

    for (size_t i = 0; i < 500; ++i) {
        auto byte = slice.value()->read_byte();
        ASSERT_TRUE(byte.has_value());
        ASSERT_EQ(byte.value(), payload[1000 + i]) << i;
    }
    // The clone still starts at 0 and sees the same window.
    auto clone_first = clone.value()->read_byte();
    ASSERT_TRUE(clone_first.has_value());
    EXPECT_EQ(clone_first.value(), payload[1000]);

    // Clone of the parent keeps a partially filled cache consistent.
    ASSERT_TRUE(input.value()->seek(700).has_value());
    ASSERT_TRUE(input.value()->read_byte().has_value());
    auto parent_clone = input.value()->clone();
    ASSERT_TRUE(parent_clone.has_value());
    auto cloned_next = parent_clone.value()->read_byte();
    ASSERT_TRUE(cloned_next.has_value());
    EXPECT_EQ(cloned_next.value(), payload[701]);

    EXPECT_FALSE(input.value()->slice("bad", 2400, 200).has_value());
}

TEST(BufferedIndexInputTest, RandomAccessReads) {
    const auto payload = make_payload(3000);
    ByteBuffersDirectory directory;
    write_memory_file(directory, payload);
    auto input = open_buffered_file(directory, 64);
    ASSERT_TRUE(input.has_value());
    auto &buffered = *input.value();

    // Positional reads must not move the sequential pointer.
    ASSERT_TRUE(buffered.seek(5).has_value());
    const auto position = buffered.get_file_pointer().value();

    // Integer reads at every offset, including unaligned ones.
    for (size_t pos = 0; pos + 4 <= payload.size(); pos += 7) {
        auto value = buffered.read_int(pos);
        ASSERT_TRUE(value.has_value()) << pos;
        ASSERT_EQ(value.value(), BitUtil::get_i32_le(payload, pos)) << pos;
    }
    // Backwards random access through the same cache.
    for (size_t pos = payload.size() - 8; pos >= 100; pos -= 61) {
        auto value = buffered.read_long(pos);
        ASSERT_TRUE(value.has_value()) << pos;
        ASSERT_EQ(value.value(), BitUtil::get_i64_le(payload, pos)) << pos;
    }
    EXPECT_EQ(buffered.get_file_pointer().value(), position);

    std::vector<uint8_t> window(200);
    ASSERT_TRUE(buffered.read_bytes(1500, window).has_value());
    EXPECT_TRUE(std::equal(window.begin(), window.end(), payload.begin() + 1500));

    EXPECT_FALSE(buffered.read_byte(payload.size()).has_value());
    EXPECT_FALSE(buffered.read_bytes(payload.size() - 1, window).has_value());

    // The random access slice is positioned relative to the slice origin.
    auto view = buffered.random_access_slice(1000, 500);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view.value()->length().value(), 500U);
    auto value = view.value()->read_int(100);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value.value(), BitUtil::get_i32_le(payload, 1100));
    EXPECT_FALSE(view.value()->read_int(497).has_value());
}

TEST(BufferedIndexInputTest, UnbufferedReadsBypassTheCache) {
    const auto payload = make_payload(3000);
    ByteBuffersDirectory directory;
    write_memory_file(directory, payload);
    auto input = open_buffered_file(directory, 1024);
    ASSERT_TRUE(input.has_value());
    auto &buffered = *input.value();

    ASSERT_TRUE(buffered.seek(0).has_value());
    std::vector<uint8_t> direct(2000);
    ASSERT_TRUE(buffered.read_bytes_with_buffer(direct, false).has_value());
    EXPECT_TRUE(std::equal(direct.begin(), direct.end(), payload.begin()));
    EXPECT_EQ(buffered.buffered_length(), 0U);
    EXPECT_EQ(buffered.get_file_pointer().value(), 2000U);
}

TEST(BufferedIndexInputTest, BufferSizeRules) {
    EXPECT_EQ(BufferedIndexInput::buffer_size_for(io_context_default()), BufferedIndexInput::BUFFER_SIZE);
    const auto flush = IOContext::with_flush(FlushInfo(1, 10));
    ASSERT_TRUE(flush.has_value());
    EXPECT_EQ(BufferedIndexInput::buffer_size_for(flush.value()), BufferedIndexInput::BUFFER_SIZE);
    const auto merge = IOContext::with_merge(MergeInfo(1, 10, false, 2));
    ASSERT_TRUE(merge.has_value());
    EXPECT_EQ(BufferedIndexInput::buffer_size_for(merge.value()), BufferedIndexInput::MERGE_BUFFER_SIZE);

    EXPECT_TRUE(BufferedIndexInput::check_buffer_size(BufferedIndexInput::MIN_BUFFER_SIZE).has_value());
    EXPECT_FALSE(BufferedIndexInput::check_buffer_size(BufferedIndexInput::MIN_BUFFER_SIZE - 1).has_value());

    const auto payload = make_payload(100);
    ByteBuffersDirectory directory;
    write_memory_file(directory, payload);
    auto input = directory.open_input(FILE_NAME, merge.value());
    ASSERT_TRUE(input.has_value());
    auto buffered = open_buffered_with_context(std::move(input.value()), std::string(FILE_NAME), merge.value());
    ASSERT_TRUE(buffered.has_value());
    EXPECT_EQ(buffered.value()->buffer_size(), BufferedIndexInput::MERGE_BUFFER_SIZE);
    EXPECT_EQ(buffered.value()->to_string(), std::string("BufferedIndexInput(") + std::string(FILE_NAME) + ")");
}

// ---------------- MemorySegmentIndexInput ----------------

/// Creates a real file with `payload` and returns an open read-only descriptor.
int write_temp_file(const std::filesystem::path &path, std::span<const uint8_t> payload) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) {
        return -1;
    }
    size_t done = 0;
    while (done < payload.size()) {
        const ssize_t written = ::pwrite(fd, payload.data() + done, payload.size() - done, static_cast<off_t>(done));
        if (written <= 0) {
            ::close(fd);
            return -1;
        }
        done += static_cast<size_t>(written);
    }
    return fd;
}

TEST(MemorySegmentIndexInputTest, ReadsAcrossChunkBoundaries) {
    const auto payload = make_payload(10000);
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_mmap_input_ut.bin";
    const int fd = write_temp_file(path, payload);
    ASSERT_GE(fd, 0);

    // Mapping chunks are page aligned, so ask for exactly one page per chunk to
    // get three mappings and exercise the cross-chunk paths.
    auto mapping = std::make_shared<MemoryMapping>();
    ASSERT_TRUE(mapping->map(fd, payload.size(), 4096).has_value());
    ::close(fd);
    ASSERT_EQ(mapping->size(), payload.size());
    ASSERT_EQ(mapping->chunk_count(), 3U);

    MemorySegmentIndexInput input(mapping, 0, payload.size(), "mmap");
    EXPECT_EQ(input.length().value(), payload.size());
    for (size_t i = 0; i < payload.size(); ++i) {
        auto byte = input.read_byte();
        ASSERT_TRUE(byte.has_value()) << i;
        ASSERT_EQ(byte.value(), payload[i]) << i;
    }
    EXPECT_FALSE(input.read_byte().has_value());

    // An array read that straddles a chunk boundary.
    ASSERT_TRUE(input.seek(4094).has_value());
    std::array<uint8_t, 10> straddle{};
    ASSERT_TRUE(input.read_bytes(straddle).has_value());
    EXPECT_TRUE(std::equal(straddle.begin(), straddle.end(), payload.begin() + 4094));

    // Scalars across a boundary.
    ASSERT_TRUE(input.seek(4094).has_value());
    auto value = input.read_int();
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value.value(), BitUtil::get_i32_le(payload, 4094));

    std::filesystem::remove(path);
}

TEST(MemorySegmentIndexInputTest, SliceCloneAndRandomAccess) {
    const auto payload = make_payload(700);
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_mmap_slice_ut.bin";
    const int fd = write_temp_file(path, payload);
    ASSERT_GE(fd, 0);
    auto mapping = std::make_shared<MemoryMapping>();
    ASSERT_TRUE(mapping->map(fd, payload.size(), 128).has_value());
    ::close(fd);

    MemorySegmentIndexInput input(mapping, 0, payload.size(), "mmap");
    auto slice = input.slice("slice", 300, 200);
    ASSERT_TRUE(slice.has_value());
    ASSERT_EQ(slice.value()->length().value(), 200U);
    auto clone = slice.value()->clone();
    ASSERT_TRUE(clone.has_value());
    ASSERT_TRUE(slice.value()->skip_bytes(100).has_value());
    auto sliced_byte = slice.value()->read_byte();
    ASSERT_TRUE(sliced_byte.has_value());
    EXPECT_EQ(sliced_byte.value(), payload[400]);
    // The clone kept position 0.
    auto cloned_byte = clone.value()->read_byte();
    ASSERT_TRUE(cloned_byte.has_value());
    EXPECT_EQ(cloned_byte.value(), payload[300]);

    auto view = input.random_access_slice(400, 250);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view.value()->length().value(), 250U);
    auto positional = view.value()->read_int(250 - 4);
    ASSERT_TRUE(positional.has_value());
    EXPECT_EQ(positional.value(), BitUtil::get_i32_le(payload, 646));
    EXPECT_FALSE(view.value()->read_byte(250).has_value());

    EXPECT_FALSE(input.slice("bad", 600, 200).has_value());
    EXPECT_FALSE(input.seek(payload.size() + 1).has_value());
    EXPECT_FALSE(input.skip_bytes(-1).has_value());
    EXPECT_EQ(input.to_string(), "MemorySegmentIndexInput(mmap)");

    std::filesystem::remove(path);
}

TEST(MemorySegmentIndexInputTest, EmptyFileIsReadableButEmpty) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_mmap_empty_ut.bin";
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    ASSERT_GE(fd, 0);
    auto mapping = std::make_shared<MemoryMapping>();
    ASSERT_TRUE(mapping->map(fd, 0, 64).has_value());
    ::close(fd);
    EXPECT_TRUE(mapping->empty());

    MemorySegmentIndexInput input(mapping, 0, 0, "empty");
    EXPECT_EQ(input.length().value(), 0U);
    EXPECT_FALSE(input.read_byte().has_value());
    auto zero_slice = input.slice("empty", 0, 0);
    ASSERT_TRUE(zero_slice.has_value());
    EXPECT_EQ(zero_slice.value()->length().value(), 0U);
    std::filesystem::remove(path);
}

// ---------------- FSDirectory integration ----------------

TEST(FSDirectoryMmapTest, OpenInputUsesMappingAndSlices) {
    const auto payload = make_payload(5000);
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_fs_mmap_ut";
    std::error_code ec;
    std::filesystem::remove_all(path, ec);

    auto directory = FSDirectory::open(path);
    ASSERT_TRUE(directory.has_value());
    EXPECT_TRUE(directory.value()->use_mmap());

    auto output = directory.value()->create_output("_0.tim", io_context_default());
    ASSERT_TRUE(output.has_value());
    ASSERT_TRUE(output.value()->write_bytes(payload).has_value());
    ASSERT_TRUE(output.value()->close().has_value());

    auto input = directory.value()->open_input("_0.tim", io_context_default());
    ASSERT_TRUE(input.has_value());
    EXPECT_EQ(input.value()->length().value(), payload.size());
    for (size_t i = 0; i < payload.size(); ++i) {
        auto byte = input.value()->read_byte();
        ASSERT_TRUE(byte.has_value()) << i;
        ASSERT_EQ(byte.value(), payload[i]) << i;
    }

    auto slice = input.value()->slice("field", 1000, 3000);
    ASSERT_TRUE(slice.has_value());
    ASSERT_TRUE(slice.value()->seek(2999).has_value());
    auto last = slice.value()->read_byte();
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last.value(), payload[3999]);

    auto view = input.value()->random_access_slice(0, payload.size());
    ASSERT_TRUE(view.has_value());
    auto value = view.value()->read_int(4996);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value.value(), BitUtil::get_i32_le(payload, 4996));

    EXPECT_FALSE(directory.value()->open_input("_9.tim", io_context_default()).has_value());
    std::filesystem::remove_all(path, ec);
}

TEST(FSDirectoryMmapTest, BufferedFallbackMatchesMapping) {
    const auto payload = make_payload(2500);
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_fs_nommap_ut";
    std::error_code ec;
    std::filesystem::remove_all(path, ec);

    auto directory = FSDirectory::open(path);
    ASSERT_TRUE(directory.has_value());
    directory.value()->set_use_mmap(false);
    EXPECT_FALSE(directory.value()->use_mmap());

    auto output = directory.value()->create_output("_0.fdx", io_context_default());
    ASSERT_TRUE(output.has_value());
    ASSERT_TRUE(output.value()->write_bytes(payload).has_value());
    ASSERT_TRUE(output.value()->close().has_value());

    auto input = directory.value()->open_input("_0.fdx", io_context_default());
    ASSERT_TRUE(input.has_value());
    std::vector<uint8_t> read_back(payload.size());
    ASSERT_TRUE(input.value()->read_bytes(read_back).has_value());
    EXPECT_TRUE(read_back == payload);

    auto clone = input.value()->clone();
    ASSERT_TRUE(clone.has_value());
    std::filesystem::remove_all(path, ec);
}

} // namespace
