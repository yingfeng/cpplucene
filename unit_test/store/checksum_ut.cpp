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
#include <memory>
#include <span>
#include <string>
#include <vector>

module lucene_core:ut.checksum;

import lucene_core;

using namespace lucene;

namespace {

const std::vector<uint8_t> CHECK_VECTOR{'1', '2', '3', '4', '5', '6', '7', '8', '9'};

/// 0xCBF43926, the standard CRC-32/ISO-HDLC check value of "123456789".
constexpr int64_t CHECK_VECTOR_CRC = 0xCBF43926;

std::vector<uint8_t> read_all(IndexInput &input) {
    auto length = input.length();
    EXPECT_TRUE(length.has_value());
    std::vector<uint8_t> bytes(length.value_or(0), 0);
    EXPECT_TRUE(input.read_bytes(bytes).has_value());
    return bytes;
}

} // namespace

TEST(Crc32ChecksumTest, MatchesTheStandardCheckVector) {
    Crc32Checksum checksum;
    checksum.update_bytes(CHECK_VECTOR, 0, CHECK_VECTOR.size());
    EXPECT_EQ(checksum.get_value(), CHECK_VECTOR_CRC);
}

TEST(Crc32ChecksumTest, ByteByByteMatchesBulk) {
    Crc32Checksum bulk;
    bulk.update_bytes(CHECK_VECTOR, 0, CHECK_VECTOR.size());

    Crc32Checksum single;
    for (uint8_t byte : CHECK_VECTOR) {
        single.update(byte);
    }
    EXPECT_EQ(single.get_value(), bulk.get_value());

    bulk.reset();
    EXPECT_EQ(bulk.get_value(), 0);
}

TEST(BufferedChecksumTest, BufferingDoesNotChangeTheDigest) {
    Crc32Checksum reference;
    reference.update_bytes(CHECK_VECTOR, 0, CHECK_VECTOR.size());

    // A buffer smaller than a long forces rlucene's direct-update path too.
    for (size_t buffer_size : {size_t{3}, size_t{8}, size_t{1024}}) {
        BufferedChecksum buffered{std::make_unique<Crc32Checksum>(), buffer_size};
        for (uint8_t byte : CHECK_VECTOR) {
            buffered.update(byte);
        }
        EXPECT_EQ(buffered.get_value(), reference.get_value()) << "buffer size " << buffer_size;
    }

    BufferedChecksum bulk{std::make_unique<Crc32Checksum>()};
    bulk.update_bytes(CHECK_VECTOR, 0, CHECK_VECTOR.size());
    EXPECT_EQ(bulk.get_value(), reference.get_value());
}

TEST(BufferedChecksumTest, PrimitiveUpdatesUseLittleEndianBytes) {
    // rlucene updates the digest with `value.to_le_bytes()`.
    const std::vector<uint8_t> little_endian{0x04, 0x03, 0x02, 0x01};
    Crc32Checksum reference;
    reference.update_bytes(little_endian, 0, little_endian.size());

    BufferedChecksum buffered{std::make_unique<Crc32Checksum>()};
    buffered.update_int(0x01020304);
    EXPECT_EQ(buffered.get_value(), reference.get_value());

    BufferedChecksum longs{std::make_unique<Crc32Checksum>()};
    const std::vector<int64_t> values{0x0102030405060708, 1};
    longs.update_longs(values, 0, values.size());
    Crc32Checksum longs_reference;
    const std::vector<uint8_t> expected{0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 1, 0, 0, 0, 0, 0, 0, 0};
    longs_reference.update_bytes(expected, 0, expected.size());
    EXPECT_EQ(longs.get_value(), longs_reference.get_value());
}

TEST(BufferedChecksumIndexInputTest, ChecksumsTheBytesItReads) {
    ByteBuffersDirectory directory;
    auto output = directory.create_output("data.bin", io_context_default());
    ASSERT_TRUE(output.has_value());
    ASSERT_TRUE(output.value()->write_bytes(CHECK_VECTOR, 0, CHECK_VECTOR.size()).has_value());
    ASSERT_TRUE(output.value()->close().has_value());

    auto input = directory.open_checksum_input("data.bin");
    ASSERT_TRUE(input.has_value());

    auto bytes = read_all(*input.value());
    EXPECT_EQ(bytes, CHECK_VECTOR);
    EXPECT_EQ(input.value()->get_checksum(), CHECK_VECTOR_CRC);

    auto position = input.value()->get_file_pointer();
    ASSERT_TRUE(position.has_value());
    EXPECT_EQ(position.value(), CHECK_VECTOR.size());

    // Forward seeks read through the bytes so the checksum stays correct.
    ASSERT_TRUE(input.value()->seek(9).has_value());
    EXPECT_EQ(input.value()->get_checksum(), CHECK_VECTOR_CRC);

    // Backward seeks are rejected.
    auto backwards = input.value()->seek(0);
    ASSERT_FALSE(backwards.has_value());
    EXPECT_EQ(backwards.error().kind(), ErrorKind::kIllegalState);

    // Cloning and random access slicing are not supported.
    auto cloned = input.value()->clone();
    ASSERT_FALSE(cloned.has_value());
    EXPECT_EQ(cloned.error().kind(), ErrorKind::kUnsupportedOperation);
    auto slice = input.value()->random_access_slice(0, 1);
    ASSERT_FALSE(slice.has_value());
    EXPECT_EQ(slice.error().kind(), ErrorKind::kUnsupportedOperation);
    EXPECT_TRUE(input.value()->close().has_value());
}

TEST(BufferedChecksumIndexInputTest, SeekForwardReadsThroughTheSkippedBytes) {
    ByteBuffersDirectory directory;
    auto output = directory.create_output("data.bin", io_context_default());
    ASSERT_TRUE(output.has_value());
    ASSERT_TRUE(output.value()->write_bytes(CHECK_VECTOR, 0, CHECK_VECTOR.size()).has_value());
    ASSERT_TRUE(output.value()->close().has_value());

    auto input = directory.open_checksum_input("data.bin");
    ASSERT_TRUE(input.has_value());
    ASSERT_TRUE(input.value()->seek(4).has_value());

    std::vector<uint8_t> rest(5, 0);
    ASSERT_TRUE(input.value()->read_bytes(rest).has_value());
    EXPECT_EQ(rest, (std::vector<uint8_t>{'5', '6', '7', '8', '9'}));
    // The four skipped bytes were read, so the checksum covers the whole file.
    EXPECT_EQ(input.value()->get_checksum(), CHECK_VECTOR_CRC);
}
