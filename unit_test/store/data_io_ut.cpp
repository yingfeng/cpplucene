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

module lucene_core:ut.data_io;

import lucene_core;

using namespace lucene;

namespace {

/// Small helper: a byte buffer with a `ByteArrayDataOutput` writing into it.
struct ScratchBuffer {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(256, 0);
    ByteArrayDataOutput out{std::span<uint8_t>{bytes}};

    [[nodiscard]] std::span<const uint8_t> written() const {
        return std::span<const uint8_t>(bytes.data(), out.get_position());
    }
};

} // namespace

TEST(DataOutputTest, PrimitiveRoundTrip) {
    ScratchBuffer buffer;
    ASSERT_TRUE(buffer.out.write_byte(0x7F).has_value());
    ASSERT_TRUE(buffer.out.write_short(-2).has_value());
    ASSERT_TRUE(buffer.out.write_int(0x01020304).has_value());
    ASSERT_TRUE(buffer.out.write_long(-1234567890123LL).has_value());

    ByteArrayDataInput in(buffer.written());
    EXPECT_EQ(in.read_byte().value(), 0x7F);
    EXPECT_EQ(in.read_short().value(), -2);
    EXPECT_EQ(in.read_int().value(), 0x01020304);
    EXPECT_EQ(in.read_long().value(), -1234567890123LL);
    EXPECT_TRUE(in.eof());
}

TEST(DataOutputTest, VIntEncodingMatchesLucene) {
    ScratchBuffer buffer;
    ASSERT_TRUE(buffer.out.write_vint(0).has_value());
    ASSERT_TRUE(buffer.out.write_vint(127).has_value());
    ASSERT_TRUE(buffer.out.write_vint(128).has_value());
    ASSERT_TRUE(buffer.out.write_vint(16383).has_value());
    ASSERT_TRUE(buffer.out.write_vint(16384).has_value());

    const auto bytes = buffer.written();
    ASSERT_EQ(bytes.size(), 1u + 1u + 2u + 2u + 3u);
    EXPECT_EQ(bytes[0], 0x00);
    EXPECT_EQ(bytes[1], 0x7F);
    EXPECT_EQ(bytes[2], 0x80);
    EXPECT_EQ(bytes[3], 0x01);
    EXPECT_EQ(bytes[4], 0xFF);
    EXPECT_EQ(bytes[5], 0x7F);
    EXPECT_EQ(bytes[6], 0x80);
    EXPECT_EQ(bytes[7], 0x80);
    EXPECT_EQ(bytes[8], 0x01);

    ByteArrayDataInput in(bytes);
    EXPECT_EQ(in.read_vint().value(), 0);
    EXPECT_EQ(in.read_vint().value(), 127);
    EXPECT_EQ(in.read_vint().value(), 128);
    EXPECT_EQ(in.read_vint().value(), 16383);
    EXPECT_EQ(in.read_vint().value(), 16384);
}

TEST(DataOutputTest, ZigZagAndVLong) {
    ScratchBuffer buffer;
    for (int32_t value : {0, -1, 1, -1000, 1000, std::numeric_limits<int32_t>::min()}) {
        ASSERT_TRUE(buffer.out.write_zint(value).has_value());
    }
    for (int64_t value : {int64_t{0}, int64_t{-1}, int64_t{1}, int64_t{1} << 40}) {
        ASSERT_TRUE(buffer.out.write_zlong(value).has_value());
    }

    ByteArrayDataInput in(buffer.written());
    for (int32_t value : {0, -1, 1, -1000, 1000, std::numeric_limits<int32_t>::min()}) {
        EXPECT_EQ(in.read_zint().value(), value);
    }
    for (int64_t value : {int64_t{0}, int64_t{-1}, int64_t{1}, int64_t{1} << 40}) {
        EXPECT_EQ(in.read_zlong().value(), value);
    }

    // Negative vlongs are rejected, matching the reference implementation.
    ScratchBuffer bad;
    auto result = bad.out.write_vlong(-1);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(DataOutputTest, StringMapAndSetRoundTrip) {
    ScratchBuffer buffer;
    ASSERT_TRUE(buffer.out.write_string("héllo").has_value());
    ASSERT_TRUE(buffer.out.write_map_of_strings({{"a", "1"}, {"b", "2"}}).has_value());
    ASSERT_TRUE(buffer.out.write_set_of_strings({"x", "y", "z"}).has_value());

    ByteArrayDataInput in(buffer.written());
    EXPECT_EQ(in.read_string().value(), "héllo");
    const auto map = in.read_map_of_strings();
    ASSERT_TRUE(map.has_value());
    EXPECT_EQ(map->size(), 2u);
    EXPECT_EQ(map->at("b"), "2");
    const auto set = in.read_set_of_strings();
    ASSERT_TRUE(set.has_value());
    EXPECT_EQ(set->size(), 3u);
    EXPECT_TRUE(set->contains("y"));
    EXPECT_TRUE(in.eof());
}

TEST(DataOutputTest, GroupVIntRoundTrip) {
    // flag=0x00 => four 1-byte values.
    const std::vector<uint8_t> encoded{0x00, 1, 2, 3, 4};
    ByteArrayDataInput in(encoded);
    std::array<int32_t, 4> values{};
    ASSERT_TRUE(in.read_group_vint(values).has_value());
    EXPECT_EQ(values[0], 1);
    EXPECT_EQ(values[1], 2);
    EXPECT_EQ(values[2], 3);
    EXPECT_EQ(values[3], 4);
    EXPECT_TRUE(in.eof());

    // flag=0xFF => four 4-byte values.
    std::vector<uint8_t> wide{0xFF};
    for (int32_t i = 0; i < 4; ++i) {
        const uint32_t value = 0x11223344u + static_cast<uint32_t>(i);
        for (int shift = 0; shift < 32; shift += 8) {
            wide.push_back(static_cast<uint8_t>(value >> shift));
        }
    }
    ByteArrayDataInput wide_in(wide);
    ASSERT_TRUE(wide_in.read_group_vint(values).has_value());
    EXPECT_EQ(values[0], 0x11223344);
    EXPECT_EQ(values[3], 0x11223347);
}

TEST(ByteArrayDataInputTest, EofAndSkip) {
    const std::vector<uint8_t> bytes{1, 2, 3};
    ByteArrayDataInput in(bytes);
    EXPECT_EQ(in.length(), 3u);
    ASSERT_TRUE(in.skip_bytes(2).has_value());
    EXPECT_EQ(in.read_byte().value(), 3);
    EXPECT_TRUE(in.eof());

    auto past_end = in.read_byte();
    ASSERT_FALSE(past_end.has_value());
    EXPECT_EQ(past_end.error().kind(), ErrorKind::kEof);

    in.rewind();
    EXPECT_EQ(in.read_byte().value(), 1);

    // Skipping past the end must fail rather than silently succeed.
    EXPECT_FALSE(in.skip_bytes(100).has_value());
}

TEST(ByteArrayDataInputTest, RangeWindow) {
    const std::vector<uint8_t> bytes{10, 20, 30, 40, 50};
    auto in = ByteArrayDataInput::with_range(bytes, 1, 3);
    EXPECT_EQ(in.length(), 4u);
    EXPECT_EQ(in.read_byte().value(), 20);
    EXPECT_EQ(in.read_byte().value(), 30);
    EXPECT_EQ(in.read_byte().value(), 40);
    EXPECT_TRUE(in.eof());
}

TEST(DataOutputTest, WritePastLimitIsRejected) {
    std::vector<uint8_t> bytes(2, 0);
    ByteArrayDataOutput out{std::span<uint8_t>{bytes}};
    ASSERT_TRUE(out.write_byte(1).has_value());
    ASSERT_TRUE(out.write_byte(2).has_value());
    auto result = out.write_byte(3);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(IOContextTest, ValidationRules) {
    auto default_ctx = IOContext::with_read_advice(ReadAdvice::Random);
    ASSERT_TRUE(default_ctx.has_value());
    EXPECT_EQ(default_ctx->context(), IOContextType::Default);
    EXPECT_EQ(default_ctx->read_advice(), ReadAdvice::Random);

    // MERGE requires merge_info.
    auto bad_merge = IOContext::create(IOContextType::Merge, ReadAdvice::Sequential, std::nullopt, std::nullopt);
    EXPECT_FALSE(bad_merge.has_value());

    // FLUSH/MERGE must use sequential advice.
    auto bad_advice = IOContext::create(IOContextType::Flush,
                                        ReadAdvice::Random,
                                        std::nullopt,
                                        FlushInfo(10, 1000));
    EXPECT_FALSE(bad_advice.has_value());

    auto flush_ctx = IOContext::with_flush(FlushInfo(10, 1000));
    ASSERT_TRUE(flush_ctx.has_value());
    EXPECT_EQ(flush_ctx->context(), IOContextType::Flush);
    ASSERT_TRUE(flush_ctx->flush_info().has_value());
    EXPECT_EQ(flush_ctx->flush_info()->num_docs, 10);

    auto merge_ctx = IOContext::with_merge(MergeInfo(5, 2048, false, 3));
    ASSERT_TRUE(merge_ctx.has_value());
    ASSERT_TRUE(merge_ctx->merge_info().has_value());
    EXPECT_EQ(merge_ctx->merge_info()->merge_max_num_segments, 3);

    EXPECT_EQ(io_context_read_once().read_advice(), ReadAdvice::Sequential);
    EXPECT_EQ(read_advice_from_str("sequential").value(), ReadAdvice::Sequential);
    EXPECT_FALSE(read_advice_from_str("bogus").has_value());
}
