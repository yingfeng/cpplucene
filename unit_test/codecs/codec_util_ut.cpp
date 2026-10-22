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
#include <string>

module lucene_core:ut.codec_util;

import lucene_core;

using namespace lucene;

namespace {

/// In-memory [`IndexOutput`] with a fixed checksum, enough for footer tests.
class FixedChecksumIndexOutput final : public IndexOutput {
public:
    explicit FixedChecksumIndexOutput(uint64_t checksum) : checksum_(checksum) {}

    [[nodiscard]] VoidResult write_byte(uint8_t b) override {
        bytes_.push_back(b);
        return {};
    }

    [[nodiscard]] VoidResult write_bytes(std::span<const uint8_t> b, size_t offset, size_t length) override {
        if (offset + length > b.size()) {
            return err(LuceneError::illegal_argument("write out of bounds"));
        }
        bytes_.insert(bytes_.end(), b.begin() + offset, b.begin() + offset + length);
        return {};
    }

    [[nodiscard]] Result<size_t> get_file_pointer() const override { return bytes_.size(); }
    [[nodiscard]] uint64_t get_checksum() override { return checksum_; }
    [[nodiscard]] std::string_view get_name() const override { return "checksum"; }
    [[nodiscard]] VoidResult close() override { return {}; }

    [[nodiscard]] const std::vector<uint8_t> &bytes() const noexcept { return bytes_; }

private:
    uint64_t checksum_;
    std::vector<uint8_t> bytes_;
};

} // namespace

TEST(CodecUtilTest, WritesAndChecksAHeader) {
    std::vector<uint8_t> buffer(64, 0);
    ByteArrayDataOutput out(buffer);
    ASSERT_TRUE(CodecUtil::write_header(out, "FST", 9).has_value());
    EXPECT_EQ(out.get_position(), CodecUtil::header_length("FST"));
    EXPECT_EQ(CodecUtil::header_length("FST"), 12u);

    ByteArrayDataInput in(std::span<const uint8_t>(buffer.data(), out.get_position()));
    auto version = CodecUtil::check_header(in, "FST", 6, 9);
    ASSERT_TRUE(version.has_value()) << (version ? "" : version.error().to_string());
    EXPECT_EQ(version.value(), 9);
    EXPECT_TRUE(in.eof());
}

TEST(CodecUtilTest, RejectsWrongCodecAndVersion) {
    std::vector<uint8_t> buffer(64, 0);
    ByteArrayDataOutput out(buffer);
    ASSERT_TRUE(CodecUtil::write_header(out, "FST", 9).has_value());
    const auto written = std::span<const uint8_t>(buffer.data(), out.get_position());

    {
        ByteArrayDataInput in(written);
        auto wrong_codec = CodecUtil::check_header(in, "TIM", 6, 9);
        ASSERT_FALSE(wrong_codec.has_value());
        EXPECT_EQ(wrong_codec.error().kind(), ErrorKind::kCorruptIndex);
    }
    {
        ByteArrayDataInput in(written);
        auto too_new = CodecUtil::check_header(in, "FST", 6, 8);
        ASSERT_FALSE(too_new.has_value());
        EXPECT_EQ(too_new.error().kind(), ErrorKind::kIndexFormatTooNew);
    }
    {
        ByteArrayDataInput in(written);
        auto too_old = CodecUtil::check_header(in, "FST", 10, 12);
        ASSERT_FALSE(too_old.has_value());
        EXPECT_EQ(too_old.error().kind(), ErrorKind::kIndexFormatTooOld);
    }
    {
        // A corrupted magic is detected before anything else.
        std::vector<uint8_t> corrupted(written.begin(), written.end());
        corrupted[0] = 0;
        ByteArrayDataInput in(corrupted);
        auto bad_magic = CodecUtil::check_header(in, "FST", 6, 9);
        ASSERT_FALSE(bad_magic.has_value());
        EXPECT_EQ(bad_magic.error().kind(), ErrorKind::kCorruptIndex);
    }
}

TEST(CodecUtilTest, RejectsInvalidCodecNames) {
    std::vector<uint8_t> buffer(512, 0);
    ByteArrayDataOutput out(buffer);
    const std::string too_long(128, 'a');
    EXPECT_FALSE(CodecUtil::write_header(out, too_long, 1).has_value());
    EXPECT_FALSE(CodecUtil::write_header(out, "FST\xC3\xA9", 1).has_value());
}

TEST(CodecUtilTest, BigEndianRoundTrip) {
    std::vector<uint8_t> buffer(64, 0);
    ByteArrayDataOutput out(buffer);
    ASSERT_TRUE(CodecUtil::write_be_int(out, -1234567).has_value());
    ASSERT_TRUE(CodecUtil::write_be_long(out, -9007199254740993LL).has_value());

    // Big endian: the first byte holds the sign bits.
    EXPECT_EQ(buffer[0], 0xFF); // -1234567 == 0xFFED2979
    EXPECT_EQ(buffer[1], 0xED);

    ByteArrayDataInput in(std::span<const uint8_t>(buffer.data(), out.get_position()));
    auto int_value = CodecUtil::read_be_int(in);
    ASSERT_TRUE(int_value.has_value());
    EXPECT_EQ(int_value.value(), -1234567);
    auto long_value = CodecUtil::read_be_long(in);
    ASSERT_TRUE(long_value.has_value());
    EXPECT_EQ(long_value.value(), -9007199254740993LL);
}

TEST(CodecUtilTest, WritesAndValidatesAFooter) {
    FixedChecksumIndexOutput out(0xDEADBEEFULL);
    ASSERT_TRUE(CodecUtil::write_footer(out).has_value());
    EXPECT_EQ(out.bytes().size(), CodecUtil::footer_length());

    const auto written = std::span<const uint8_t>(out.bytes().data(), out.bytes().size());
    {
        ByteArrayDataInput in(written);
        auto checksum = CodecUtil::check_footer(in);
        ASSERT_TRUE(checksum.has_value()) << (checksum ? "" : checksum.error().to_string());
        EXPECT_EQ(checksum.value(), 0xDEADBEEFLL);
        EXPECT_TRUE(in.eof());
    }
    {
        ByteArrayDataInput in(written);
        auto matching = CodecUtil::check_footer(in, 0xDEADBEEFLL);
        ASSERT_TRUE(matching.has_value()) << (matching ? "" : matching.error().to_string());
        EXPECT_EQ(matching.value(), 0xDEADBEEFLL);
    }
    {
        ByteArrayDataInput in(written);
        auto mismatched = CodecUtil::check_footer(in, 0x1234);
        ASSERT_FALSE(mismatched.has_value());
        EXPECT_EQ(mismatched.error().kind(), ErrorKind::kCorruptIndex);
    }
    {
        // A truncated footer fails while reading the CRC; the byte-array input
        // reports that as end-of-stream rather than as corruption.
        std::vector<uint8_t> truncated(out.bytes().begin(), out.bytes().end() - 4);
        ByteArrayDataInput in(truncated);
        auto bad = CodecUtil::check_footer(in);
        ASSERT_FALSE(bad.has_value());
        EXPECT_EQ(bad.error().kind(), ErrorKind::kEof);
    }
}
