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

module lucene_core:ut.compress;

import lucene_core;

using namespace lucene;

namespace {

std::vector<uint8_t> bytes_of(std::string_view text) {
    return std::vector<uint8_t>(text.begin(), text.end());
}

std::vector<uint8_t> lz4_compress(std::span<const uint8_t> data, Lz4HashTable &table) {
    std::vector<uint8_t> buffer(data.size() * 2 + 32);
    ByteArrayDataOutput output(buffer);
    auto result = LZ4::compress(data, 0, static_cast<int32_t>(data.size()), output, table);
    EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
    buffer.resize(output.get_position());
    return buffer;
}

std::vector<uint8_t> lz4_decompress(std::span<const uint8_t> compressed, size_t decompressed_len) {
    std::vector<uint8_t> restored(decompressed_len);
    ByteArrayDataInput input(compressed);
    auto result = LZ4::decompress(input, static_cast<int32_t>(decompressed_len), restored, 0);
    EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
    EXPECT_EQ(static_cast<size_t>(result.value_or(0)), decompressed_len);
    return restored;
}

} // namespace

TEST(Lz4Test, RoundTripsRepetitiveData) {
    std::string text;
    for (int i = 0; i < 50; ++i) {
        text.append("the quick brown fox jumps over the lazy dog ");
    }
    const auto original = bytes_of(text);

    FastCompressionHashTable fast;
    const auto compressed = lz4_compress(original, fast);
    EXPECT_LT(compressed.size(), original.size());
    EXPECT_EQ(lz4_decompress(compressed, original.size()), original);
}

TEST(Lz4Test, RoundTripsShortMatchesAndLiterals) {
    // A single repeated byte gives a match distance of 1, the worst case for the
    // overlapping-copy logic.
    std::vector<uint8_t> run(2000, 'a');
    run[1000] = 'b';
    for (int i = 0; i < 40; ++i) {
        run.push_back(static_cast<uint8_t>(i));
    }

    FastCompressionHashTable fast;
    EXPECT_EQ(lz4_decompress(lz4_compress(run, fast), run.size()), run);

    HighCompressionHashTable high;
    EXPECT_EQ(lz4_decompress(lz4_compress(run, high), run.size()), run);
}

TEST(Lz4Test, RoundTripsHighCompressionOnLargeInput) {
    // Larger than 64kB so the 32-bit hash table variant is exercised.
    std::string text;
    for (int i = 0; i < 5000; ++i) {
        text.append("lorem ipsum dolor sit amet ");
    }
    const auto original = bytes_of(text);
    ASSERT_GT(original.size(), 1u << 16);

    HighCompressionHashTable high;
    const auto compressed = lz4_compress(original, high);
    EXPECT_LT(compressed.size(), original.size());
    EXPECT_EQ(lz4_decompress(compressed, original.size()), original);

    FastCompressionHashTable fast;
    EXPECT_EQ(lz4_decompress(lz4_compress(original, fast), original.size()), original);
}

TEST(Lz4Test, RoundTripsWithPresetDictionary) {
    const auto dictionary = bytes_of("dictionary bytes shared by every block of the same file ");
    const auto data = bytes_of("dictionary bytes shared by every block of the same file and more content");

    std::vector<uint8_t> buffer(data.size() * 2 + 32);
    ByteArrayDataOutput output(buffer);
    FastCompressionHashTable table;
    auto compressed = LZ4::compress_with_dictionary(data, 0, static_cast<int32_t>(dictionary.size()),
                                                    static_cast<int32_t>(data.size() - dictionary.size()), output, table);
    ASSERT_TRUE(compressed.has_value()) << (compressed ? "" : compressed.error().to_string());
    buffer.resize(output.get_position());

    // The decoder must find the dictionary right before `d_off`.
    std::vector<uint8_t> restored(dictionary.size() + (data.size() - dictionary.size()));
    std::copy(dictionary.begin(), dictionary.end(), restored.begin());
    ByteArrayDataInput input(buffer);
    auto result = LZ4::decompress(input, static_cast<int32_t>(data.size() - dictionary.size()), restored,
                                  static_cast<int32_t>(dictionary.size()));
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
    EXPECT_EQ(std::vector<uint8_t>(restored.begin(), restored.end()), data);
}

TEST(Lz4Test, RejectsDictionaryLargerThanTheWindow) {
    std::vector<uint8_t> data(200000, 'x');
    std::vector<uint8_t> buffer(data.size() * 2);
    ByteArrayDataOutput output(buffer);
    FastCompressionHashTable table;
    auto result = LZ4::compress_with_dictionary(data, 0, LZ4::MAX_DISTANCE + 1, 10, output, table);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(LowercaseAsciiCompressionTest, RoundTripsLowercaseContent) {
    // One exception (`Z`) is allowed for this length: `len >> 5` == 1.
    const auto original = bytes_of("abcdefghijklmnopqrstuvwxyzZ1234567890-_.");
    std::vector<uint8_t> tmp(original.size());
    std::vector<uint8_t> buffer(original.size() * 2);
    ByteArrayDataOutput output(buffer);

    auto compressed = LowercaseAsciiCompression::compress(original, original.size(), tmp, output);
    ASSERT_TRUE(compressed.has_value()) << (compressed ? "" : compressed.error().to_string());
    ASSERT_TRUE(compressed.value());
    buffer.resize(output.get_position());
    EXPECT_LT(buffer.size(), original.size());

    std::vector<uint8_t> restored(original.size());
    ByteArrayDataInput input(buffer);
    auto result = LowercaseAsciiCompression::decompress(input, restored, original.size());
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
    EXPECT_EQ(restored, original);
}

TEST(LowercaseAsciiCompressionTest, HandlesExceptionDeltasAboveOneByte) {
    // Mostly compressible, with two exceptions more than 255 bytes apart so an
    // artificial exception has to be emitted.
    std::vector<uint8_t> original(800, 'a');
    original[10] = 'A';
    original[400] = 'B';

    std::vector<uint8_t> tmp(original.size());
    std::vector<uint8_t> buffer(original.size() * 2);
    ByteArrayDataOutput output(buffer);
    auto compressed = LowercaseAsciiCompression::compress(original, original.size(), tmp, output);
    ASSERT_TRUE(compressed.has_value()) << (compressed ? "" : compressed.error().to_string());
    ASSERT_TRUE(compressed.value()) << "800 lowercase bytes with 2 exceptions must be compressible";
    buffer.resize(output.get_position());

    std::vector<uint8_t> restored(original.size());
    ByteArrayDataInput input(buffer);
    auto result = LowercaseAsciiCompression::decompress(input, restored, original.size());
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
    EXPECT_EQ(restored, original);
}

TEST(LowercaseAsciiCompressionTest, RefusesIncompressibleContent) {
    const auto uppercase = bytes_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    std::vector<uint8_t> tmp(uppercase.size());
    std::vector<uint8_t> buffer(uppercase.size() * 2);
    ByteArrayDataOutput output(buffer);
    auto compressed = LowercaseAsciiCompression::compress(uppercase, uppercase.size(), tmp, output);
    ASSERT_TRUE(compressed.has_value());
    EXPECT_FALSE(compressed.value());

    // Inputs shorter than 8 bytes are never worth compressing.
    const auto tiny = bytes_of("abc");
    auto too_short = LowercaseAsciiCompression::compress(tiny, tiny.size(), tmp, output);
    ASSERT_TRUE(too_short.has_value());
    EXPECT_FALSE(too_short.value());
}

TEST(Lz4Test, RoundTripsAMatchLongerThanSixtyFourKilobytes) {
    // A run of identical bytes compresses into one very long match.  Rounding its
    // length to a multiple of eight must keep the whole length: masking with
    // `0xFFF8` would drop the high bits and copy just a few kilobytes.
    std::vector<uint8_t> original(200000, static_cast<uint8_t>('a'));
    FastCompressionHashTable table;
    const std::vector<uint8_t> compressed = lz4_compress(original, table);
    EXPECT_LT(compressed.size(), original.size() / 10);
    EXPECT_EQ(lz4_decompress(compressed, original.size()), original);
}
