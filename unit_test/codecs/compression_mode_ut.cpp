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

#include <algorithm>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

module lucene_core:ut.compression_mode;

import lucene_core;

using namespace lucene;

namespace {

/// A `DataOutput` that appends into a `std::vector`.
class VectorDataOutput final : public DataOutput {
public:
    [[nodiscard]] VoidResult write_byte(uint8_t b) override {
        bytes.push_back(b);
        return {};
    }

    [[nodiscard]] VoidResult write_bytes(std::span<const uint8_t> b, size_t offset, size_t length) override {
        if (offset > b.size() || length > b.size() - offset) {
            return err(LuceneError::array_index_out_of_bounds("the write range is outside the buffer"));
        }
        bytes.insert(bytes.end(), b.begin() + static_cast<std::ptrdiff_t>(offset),
                     b.begin() + static_cast<std::ptrdiff_t>(offset + length));
        return {};
    }

    std::vector<uint8_t> bytes;
};

/// Compressible data: runs of `period` distinct bytes.
std::vector<uint8_t> make_payload(size_t size, size_t period = 7) {
    std::vector<uint8_t> payload(size);
    for (size_t i = 0; i < size; ++i) {
        payload[i] = static_cast<uint8_t>('a' + (i % period));
    }
    return payload;
}

/// Compresses `payload` with `mode` and returns the bytes written.
std::vector<uint8_t> compress(const CompressionMode &mode, std::span<const uint8_t> payload) {
    auto compressor = mode.new_compressor();
    EXPECT_NE(compressor, nullptr);
    VectorDataOutput out;
    auto written = compressor->compress(payload, out);
    EXPECT_TRUE(written.has_value()) << (written ? "" : written.error().to_string());
    return out.bytes;
}

/// Decompresses `[offset, offset + length)` of a chunk of `original_length`.
std::vector<uint8_t> decompress(const CompressionMode &mode, std::span<const uint8_t> compressed,
                                int32_t original_length, int32_t offset, int32_t length) {
    auto decompressor = mode.new_decompressor();
    EXPECT_NE(decompressor, nullptr);
    ByteArrayDataInput input(compressed);
    std::vector<uint8_t> dest;
    auto decompressed = decompressor->decompress(input, original_length, offset, length, dest);
    EXPECT_TRUE(decompressed.has_value()) << (decompressed ? "" : decompressed.error().to_string());
    if (dest.size() < static_cast<size_t>(offset + length)) {
        return {};
    }
    return std::vector<uint8_t>(dest.begin() + offset, dest.begin() + offset + length);
}

} // namespace

TEST(CompressionModeTest, NamesMatchTheReference) {
    EXPECT_EQ(CompressionMode::fast().name(), "FAST");
    EXPECT_EQ(CompressionMode::fast_decompression().name(), "FAST_DECOMPRESSION");
    EXPECT_EQ(CompressionMode::best_speed().name(), "BEST_SPEED");
    EXPECT_EQ(CompressionMode::best_speed().type(), CompressionModeType::BestSpeed);
    EXPECT_NE(CompressionMode::best_speed().new_compressor(), nullptr);
    EXPECT_NE(CompressionMode::best_speed().new_decompressor(), nullptr);
    EXPECT_EQ(CompressionMode::high_compression().name(), "BEST_COMPRESSION");
    EXPECT_EQ(CompressionMode::high_compression().type(), CompressionModeType::HighCompression);
    EXPECT_NE(CompressionMode::high_compression().new_compressor(), nullptr);
    EXPECT_NE(CompressionMode::high_compression().new_decompressor(), nullptr);
}

TEST(CompressionModeTest, EveryModeRoundTripsTheWholeChunk) {
    const auto payload = make_payload(5000);
    for (const CompressionMode &mode :
         {CompressionMode::fast(), CompressionMode::fast_decompression(), CompressionMode::best_speed()}) {
        const auto compressed = compress(mode, payload);
        ASSERT_FALSE(compressed.empty()) << mode.name();
        const auto restored = decompress(mode, compressed, static_cast<int32_t>(payload.size()), 0,
                                         static_cast<int32_t>(payload.size()));
        ASSERT_EQ(restored.size(), payload.size()) << mode.name();
        EXPECT_TRUE(std::equal(restored.begin(), restored.end(), payload.begin())) << mode.name();
    }
}

/// The preset dictionary mode has to reproduce the bytes a caller asks for, not
/// only the whole chunk: seed `dest` with garbage first so a partially filled
/// buffer shows up.
TEST(CompressionModeTest, BestSpeedRoundTripsPartialRanges) {
    const auto payload = make_payload(4096, 13);
    const auto compressed = compress(CompressionMode::best_speed(), payload);
    ASSERT_FALSE(compressed.empty());

    struct Range {
        int32_t offset;
        int32_t length;
    };
    for (const Range range : {Range{0, 1}, Range{0, 100}, Range{400, 7}, Range{2048, 512}, Range{4095, 1}}) {
        const auto restored = decompress(CompressionMode::best_speed(), compressed,
                                         static_cast<int32_t>(payload.size()), range.offset, range.length);
        ASSERT_EQ(restored.size(), static_cast<size_t>(range.length)) << range.offset;
        EXPECT_TRUE(std::equal(restored.begin(), restored.end(), payload.begin() + range.offset))
                << "offset " << range.offset;
    }
}

/// A chunk smaller than twenty bytes has an empty dictionary, and one that is
/// exactly the sub block bound still has to decode.
TEST(CompressionModeTest, BestSpeedHandlesTinyAndBoundaryChunks) {
    for (const size_t size : {size_t{1}, size_t{19}, size_t{20}, size_t{21}, size_t{100}}) {
        const auto payload = make_payload(size, 3);
        const auto compressed = compress(CompressionMode::best_speed(), payload);
        const auto restored = decompress(CompressionMode::best_speed(), compressed, static_cast<int32_t>(size), 0,
                                         static_cast<int32_t>(size));
        ASSERT_EQ(restored.size(), size) << "size " << size;
        EXPECT_TRUE(std::equal(restored.begin(), restored.end(), payload.begin())) << "size " << size;
    }
}

/// An empty range is a no-op, and a range outside the chunk is rejected.
TEST(CompressionModeTest, BestSpeedRejectsBadRanges) {
    const auto payload = make_payload(500, 5);
    const auto compressed = compress(CompressionMode::best_speed(), payload);

    auto decompressor = CompressionMode::best_speed().new_decompressor();
    ByteArrayDataInput input(compressed);
    std::vector<uint8_t> dest;
    EXPECT_TRUE(decompressor->decompress(input, 500, 0, 0, dest).has_value());

    ByteArrayDataInput second(compressed);
    EXPECT_FALSE(decompressor->decompress(second, 500, 400, 200, dest).has_value());
}

/// A chunk larger than `LZ4::MAX_DISTANCE` clamps the dictionary to the window
/// size; the whole chunk still has to round-trip, as do ranges deep inside it.
TEST(CompressionModeTest, BestSpeedHandlesChunksLargerThanTheWindow) {
    const auto payload = make_payload(2 * 1024 * 1024, 251);
    const auto compressed = compress(CompressionMode::best_speed(), payload);
    ASSERT_FALSE(compressed.empty());

    const auto whole = decompress(CompressionMode::best_speed(), compressed,
                                  static_cast<int32_t>(payload.size()), 0,
                                  static_cast<int32_t>(payload.size()));
    ASSERT_EQ(whole.size(), payload.size());
    EXPECT_TRUE(std::equal(whole.begin(), whole.end(), payload.begin()));

    // Ranges near the end, well past the dictionary, use the later sub blocks.
    for (const int32_t offset : {1'000'000, 1'800'000}) {
        const auto part = decompress(CompressionMode::best_speed(), compressed,
                                     static_cast<int32_t>(payload.size()), offset, 64);
        ASSERT_EQ(part.size(), 64U) << offset;
        EXPECT_TRUE(std::equal(part.begin(), part.end(), payload.begin() + offset)) << offset;
    }
}

/// `HIGH_COMPRESSION` splits a chunk exactly like `BEST_SPEED`, but every block is
/// a Deflate stream, so the whole chunk, partial ranges and tiny chunks all have
/// to come back unchanged.
TEST(CompressionModeTest, HighCompressionRoundTripsLikeTheLz4Modes) {
    for (const size_t size : {size_t{1}, size_t{19}, size_t{20}, size_t{21}, size_t{100}, size_t{5000}, size_t{100000}}) {
        const auto payload = make_payload(size, 7);
        const auto compressed = compress(CompressionMode::high_compression(), payload);
        ASSERT_FALSE(compressed.empty()) << size;
        const auto restored = decompress(CompressionMode::high_compression(), compressed,
                                         static_cast<int32_t>(payload.size()), 0,
                                         static_cast<int32_t>(payload.size()));
        ASSERT_EQ(restored.size(), payload.size()) << size;
        EXPECT_TRUE(std::equal(restored.begin(), restored.end(), payload.begin())) << size;
    }
}

TEST(CompressionModeTest, HighCompressionRoundTripsPartialRanges) {
    const auto payload = make_payload(4096, 29);
    const auto compressed = compress(CompressionMode::high_compression(), payload);
    ASSERT_FALSE(compressed.empty());

    struct Range {
        int32_t offset;
        int32_t length;
    };
    for (const Range range : {Range{0, 1}, Range{0, 100}, Range{400, 7}, Range{2048, 512}, Range{4095, 1}}) {
        const auto restored = decompress(CompressionMode::high_compression(), compressed,
                                         static_cast<int32_t>(payload.size()), range.offset, range.length);
        ASSERT_EQ(restored.size(), static_cast<size_t>(range.length)) << range.offset;
        EXPECT_TRUE(std::equal(restored.begin(), restored.end(), payload.begin() + range.offset))
                << "offset " << range.offset;
    }
}

/// Repetitive data is what the mode is for: it has to be smaller than the LZ4
/// mode, and it still has to decode.
TEST(CompressionModeTest, HighCompressionBeatsBestSpeedOnRepetitiveData) {
    std::vector<uint8_t> payload;
    payload.reserve(64 * 1024);
    for (int32_t i = 0; i < 64 * 1024; ++i) {
        payload.push_back(static_cast<uint8_t>('a' + (i % 4)));
    }
    const auto deflate = compress(CompressionMode::high_compression(), payload);
    const auto lz4 = compress(CompressionMode::best_speed(), payload);
    ASSERT_FALSE(deflate.empty());
    ASSERT_FALSE(lz4.empty());
    EXPECT_LT(deflate.size(), lz4.size());

    const auto restored = decompress(CompressionMode::high_compression(), deflate,
                                     static_cast<int32_t>(payload.size()), 0,
                                     static_cast<int32_t>(payload.size()));
    ASSERT_EQ(restored.size(), payload.size());
    EXPECT_TRUE(std::equal(restored.begin(), restored.end(), payload.begin()));
}
