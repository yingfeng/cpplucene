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
#include <vector>

module lucene_core:ut.bkd_doc_ids_writer;

import lucene_core;

using namespace lucene;

namespace {

/// Writes `doc_ids`, checks the chosen encoding and reads the ids back.
void round_trip(const std::vector<int32_t> &doc_ids, int8_t expected_tag) {
    GrowableByteArrayDataOutput out;
    auto tag = BkdDocIdsWriter::write_doc_ids(doc_ids, out);
    ASSERT_TRUE(tag.has_value()) << (tag ? "" : tag.error().to_string());
    EXPECT_EQ(tag.value(), expected_tag);

    ByteArrayDataInput in(out.bytes());
    std::vector<int32_t> restored(doc_ids.size(), -1);
    ASSERT_TRUE(BkdDocIdsWriter::read_ints(in, doc_ids.size(), restored).has_value());
    EXPECT_EQ(restored, doc_ids);
    // The reader consumes the whole encoding and nothing more.
    EXPECT_TRUE(in.eof());
}

/// Writes `doc_ids` and compares the exact bytes with `expected`.
void expect_bytes(const std::vector<int32_t> &doc_ids, const std::vector<uint8_t> &expected) {
    GrowableByteArrayDataOutput out;
    auto tag = BkdDocIdsWriter::write_doc_ids(doc_ids, out);
    ASSERT_TRUE(tag.has_value()) << (tag ? "" : tag.error().to_string());
    const auto bytes = out.bytes();
    EXPECT_EQ(std::vector<uint8_t>(bytes.begin(), bytes.end()), expected);
}

} // namespace

TEST(BkdDocIdsWriterTest, RoundTripsContinuousIds) {
    round_trip({40, 41, 42, 43, 44}, BkdDocIdsWriter::CONTINUOUS_IDS);
}

TEST(BkdDocIdsWriterTest, RoundTripsBitsets) {
    // Sorted, dense enough for a bit set but not contiguous.
    round_trip({10, 12, 14, 16}, BkdDocIdsWriter::BITSET_IDS);
    // A bit set spanning several words: 40 ids spread over 600 values keeps the
    // span under `16 * count`, which is the threshold for choosing it.
    std::vector<int32_t> wide;
    for (int32_t i = 0; i < 40; ++i) {
        wide.push_back(i * 15);
    }
    round_trip(wide, BkdDocIdsWriter::BITSET_IDS);
    // Doc ids that do not start at zero: the stored word offset is what keeps the
    // bit set from having to begin at word 0.
    round_trip({100000, 100002, 100003, 100010}, BkdDocIdsWriter::BITSET_IDS);
}

TEST(BkdDocIdsWriterTest, BitsetThresholdIsSixteenTimesTheCount) {
    // span == count * 16 is still a bit set: {0, 1, 2, 63} spans 64 values.
    round_trip({0, 1, 2, 63}, BkdDocIdsWriter::BITSET_IDS);
    // One value more of span pushes the same count into the delta encoding.
    round_trip({0, 1, 2, 64}, BkdDocIdsWriter::DELTA_BPV_16);
    // Dense but not ascending: the sorted-only encodings must not be used, even
    // though the span equals the count.
    round_trip({0, 2, 1, 3}, BkdDocIdsWriter::DELTA_BPV_16);
}

TEST(BkdDocIdsWriterTest, RoundTripsDeltaSixteen) {
    // Unsorted, so the sorted-only encodings are off the table.
    round_trip({7, 3, 11, 0, 999, 4}, BkdDocIdsWriter::DELTA_BPV_16);
    // Odd count exercises the trailing short.
    round_trip({7, 3, 11, 0, 999}, BkdDocIdsWriter::DELTA_BPV_16);
}

TEST(BkdDocIdsWriterTest, RoundTripsTwentyFourBitsPerValue) {
    round_trip({7, 3, 100000, 0, 99999, 4}, BkdDocIdsWriter::BPV_24);
    // Sixteen ids go through the three-longs-at-a-time path.
    std::vector<int32_t> many;
    for (int32_t i = 0; i < 16; ++i) {
        many.push_back(i % 2 == 0 ? i : 200000 - i);
    }
    round_trip(many, BkdDocIdsWriter::BPV_24);
    // A tail smaller than eight after the packed longs.
    std::vector<int32_t> eleven(many.begin(), many.begin() + 11);
    round_trip(eleven, BkdDocIdsWriter::BPV_24);
}

TEST(BkdDocIdsWriterTest, RoundTripsThirtyTwoBitsPerValue) {
    round_trip({1, 20000000, 2}, BkdDocIdsWriter::BPV_32);
}

TEST(BkdDocIdsWriterTest, ReadsLegacyDeltaVints) {
    GrowableByteArrayDataOutput out;
    ASSERT_TRUE(out.write_byte(static_cast<uint8_t>(BkdDocIdsWriter::LEGACY_DELTA_VINT)).has_value());
    ASSERT_TRUE(out.write_vint(5).has_value());
    ASSERT_TRUE(out.write_vint(3).has_value());
    ASSERT_TRUE(out.write_vint(10).has_value());

    ByteArrayDataInput in(out.bytes());
    std::vector<int32_t> doc_ids(3, 0);
    ASSERT_TRUE(BkdDocIdsWriter::read_ints(in, 3, doc_ids).has_value());
    EXPECT_EQ(doc_ids, (std::vector<int32_t>{5, 8, 18}));
}

TEST(BkdDocIdsWriterTest, ReadsAnEmptyDeltaSixteenBlock) {
    // Two ids packed into one int, then nothing else.
    GrowableByteArrayDataOutput out;
    ASSERT_TRUE(out.write_byte(static_cast<uint8_t>(BkdDocIdsWriter::DELTA_BPV_16)).has_value());
    ASSERT_TRUE(out.write_vint(100).has_value());
    ASSERT_TRUE(out.write_int(static_cast<int32_t>((2u << 16) | 1u)).has_value());

    ByteArrayDataInput in(out.bytes());
    std::vector<int32_t> doc_ids(2, 0);
    ASSERT_TRUE(BkdDocIdsWriter::read_ints(in, 2, doc_ids).has_value());
    EXPECT_EQ(doc_ids, (std::vector<int32_t>{102, 101}));
}

TEST(BkdDocIdsWriterTest, WritesTheExpectedBytes) {
    // A round trip cannot catch a bug that the writer and the reader share, so
    // pin the layout for the encodings whose framing is easy to get wrong.
    //
    // Continuous: tag, then the seed as a vint.
    expect_bytes({40, 41, 42}, {0xFE, 0x28});

    // Bit set: tag, offset words (0), word count (1), then one little endian
    // word with bits 10, 12, 14 and 16 set (1048576 + 16384 + 4096 + 1024).
    expect_bytes({10, 12, 14, 16}, {0xFF, 0x00, 0x01, 0x00, 0x54, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00});

    // Bit set with a non-zero offset: 100000 >> 6 == 1562 words of 64 ids, so
    // the ids land at bits 32, 34, 35 and 42 of the first word.
    expect_bytes({100000, 100002, 100003, 100010},
                 {0xFF, 0x9A, 0x0C, 0x01, 0x00, 0x00, 0x00, 0x00, 0x0D, 0x04, 0x00, 0x00});

    // Delta 16: tag, min (0), then two 16-bit deltas per int, high half first.
    expect_bytes({7, 3, 11, 0, 999, 4}, {0x10, 0x00, 0x00, 0x00, 0x07, 0x00, 0xE7, 0x03, 0x03, 0x00,
                                         0x04, 0x00, 0x0B, 0x00});

    // Raw 32-bit ids, little endian in this port: 20000000 == 0x01312D00.
    expect_bytes({1, 20000000, 2}, {0x20, 0x01, 0x00, 0x00, 0x00, 0x00, 0x2D, 0x31, 0x01, 0x02, 0x00,
                                    0x00, 0x00});
}

TEST(BkdDocIdsWriterTest, RejectsUnknownEncodings) {
    GrowableByteArrayDataOutput out;
    ASSERT_TRUE(out.write_byte(0x7F).has_value());

    ByteArrayDataInput in(out.bytes());
    std::vector<int32_t> doc_ids(2, 0);
    auto result = BkdDocIdsWriter::read_ints(in, 2, doc_ids);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind(), ErrorKind::kCorruptIndex);
}
