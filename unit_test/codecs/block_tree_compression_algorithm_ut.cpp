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
#include <iterator>
#include <span>
#include <string>
#include <vector>

module lucene_core:ut.block_tree_compression_algorithm;

import lucene_core;

using namespace lucene;

namespace {

std::span<const uint8_t> as_bytes(const std::string &text) {
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

} // namespace

TEST(CompressionAlgorithmTest, CodesRoundTrip) {
    EXPECT_EQ(compression_algorithm_code(CompressionAlgorithm::NoCompression), 0x00);
    EXPECT_EQ(compression_algorithm_code(CompressionAlgorithm::LowercaseAscii), 0x01);
    EXPECT_EQ(compression_algorithm_code(CompressionAlgorithm::Lz4), 0x02);

    auto no_compression = compression_algorithm_by_code(0x00);
    ASSERT_TRUE(no_compression.has_value());
    EXPECT_EQ(no_compression.value(), CompressionAlgorithm::NoCompression);

    auto lowercase = compression_algorithm_by_code(0x01);
    ASSERT_TRUE(lowercase.has_value());
    EXPECT_EQ(lowercase.value(), CompressionAlgorithm::LowercaseAscii);

    auto lz4 = compression_algorithm_by_code(0x02);
    ASSERT_TRUE(lz4.has_value());
    EXPECT_EQ(lz4.value(), CompressionAlgorithm::Lz4);
}

TEST(CompressionAlgorithmTest, RejectsUnknownCodes) {
    auto unknown = compression_algorithm_by_code(0x03);
    ASSERT_FALSE(unknown.has_value());
    EXPECT_EQ(unknown.error().kind(), ErrorKind::kIllegalArgument);
    EXPECT_NE(unknown.error().message().find("Illegal code for a compression algorithm"), std::string::npos);
}

TEST(CompressionAlgorithmTest, NamesMatchTheDebugOutput) {
    EXPECT_EQ(compression_algorithm_name(CompressionAlgorithm::NoCompression), "NoCompression");
    EXPECT_EQ(compression_algorithm_name(CompressionAlgorithm::LowercaseAscii), "LowercaseAscii");
    EXPECT_EQ(compression_algorithm_name(CompressionAlgorithm::Lz4), "Lz4");
}

TEST(CompressionAlgorithmTest, ReadsUncompressedBytes) {
    const std::vector<uint8_t> raw{1, 2, 3, 4, 5};
    ByteArrayDataInput input{raw};
    std::vector<uint8_t> decoded(raw.size(), 0);
    ASSERT_TRUE(read_compressed(CompressionAlgorithm::NoCompression, input, decoded, 5).has_value());
    EXPECT_EQ(decoded, raw);
    EXPECT_EQ(input.get_position(), raw.size());
}

TEST(CompressionAlgorithmTest, ReadsLowercaseAsciiBytes) {
    const std::string text = "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz0123456789";
    std::vector<uint8_t> tmp(text.size(), 0);
    GrowableByteArrayDataOutput compressed;
    auto written = LowercaseAsciiCompression::compress(as_bytes(text), text.size(), tmp, compressed);
    ASSERT_TRUE(written.has_value());
    ASSERT_TRUE(written.value());

    ByteArrayDataInput input{compressed.bytes()};
    std::vector<uint8_t> decoded(text.size(), 0);
    ASSERT_TRUE(read_compressed(CompressionAlgorithm::LowercaseAscii, input, decoded,
                               static_cast<int32_t>(text.size()))
                        .has_value());
    EXPECT_EQ(std::string(decoded.begin(), decoded.end()), text);
}
