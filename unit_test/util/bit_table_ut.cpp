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

module lucene_core:ut.bit_table;

import lucene_core;

using namespace lucene;

namespace {

/// A [`FstBytesReader`] over a byte buffer whose position may move both ways.
class BufferReader final : public FstBytesReader {
public:
    explicit BufferReader(std::span<const uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] Result<uint8_t> read_byte() override {
        if (pos_ < 0 || static_cast<size_t>(pos_) >= bytes_.size()) {
            return err(LuceneError::eof("read past the end"));
        }
        return bytes_[static_cast<size_t>(pos_++)];
    }

    [[nodiscard]] VoidResult read_bytes(std::span<uint8_t> b) override {
        for (auto &byte : b) {
            auto value = read_byte();
            if (!value.has_value()) {
                return err(value.error());
            }
            byte = value.value();
        }
        return {};
    }

    [[nodiscard]] VoidResult skip_bytes(int64_t num_bytes) override {
        pos_ += num_bytes;
        return {};
    }

    [[nodiscard]] int64_t get_position() const override { return pos_; }
    void set_position(int64_t pos) override { pos_ = pos; }

private:
    std::span<const uint8_t> bytes_;
    int64_t pos_{0};
};

} // namespace

TEST(BitTableUtilTest, CountsAndSearchesBits) {
    // Bits 0, 3 and 17 set: bytes[0] = 0b0000'1001, bytes[2] = 0b0000'0010.
    // The buffer is padded to 8 bytes because `count_bits_upto` reads whole
    // 64-bit blocks, exactly like Lucene (real tables are always followed by
    // arc data).
    const std::vector<uint8_t> table{0x09, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
    BufferReader reader(table);

    for (const int32_t bit : {0, 3, 17}) {
        reader.set_position(0);
        auto set = BitTableUtil::is_bit_set(bit, reader);
        ASSERT_TRUE(set.has_value()) << (set ? "" : set.error().to_string());
        EXPECT_TRUE(set.value()) << "bit " << bit;
    }
    for (const int32_t bit : {1, 2, 8, 16, 31}) {
        reader.set_position(0);
        auto set = BitTableUtil::is_bit_set(bit, reader);
        ASSERT_TRUE(set.has_value());
        EXPECT_FALSE(set.value()) << "bit " << bit;
    }

    reader.set_position(0);
    auto total = BitTableUtil::count_bits(4, reader);
    ASSERT_TRUE(total.has_value());
    EXPECT_EQ(total.value(), 3);

    reader.set_position(0);
    auto upto_4 = BitTableUtil::count_bits_upto(4, reader);
    ASSERT_TRUE(upto_4.has_value());
    EXPECT_EQ(upto_4.value(), 2); // bits 0 and 3

    reader.set_position(0);
    auto upto_64 = BitTableUtil::count_bits_upto(32, reader);
    ASSERT_TRUE(upto_64.has_value());
    EXPECT_EQ(upto_64.value(), 3);

    reader.set_position(0);
    auto next = BitTableUtil::next_bit_set(1, 4, reader);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next.value(), 3);

    reader.set_position(0);
    auto none = BitTableUtil::next_bit_set(18, 4, reader);
    ASSERT_TRUE(none.has_value());
    EXPECT_EQ(none.value(), -1);

    reader.set_position(0);
    auto previous = BitTableUtil::previous_bit_set(17, reader);
    ASSERT_TRUE(previous.has_value());
    EXPECT_EQ(previous.value(), 3);

    reader.set_position(0);
    auto before_first = BitTableUtil::previous_bit_set(0, reader);
    ASSERT_TRUE(before_first.has_value());
    EXPECT_EQ(before_first.value(), -1);
}

TEST(BitTableUtilTest, PresenceBytesRoundsUpToWholeBytes) {
    EXPECT_EQ(get_num_presence_bytes(1), 1);
    EXPECT_EQ(get_num_presence_bytes(8), 1);
    EXPECT_EQ(get_num_presence_bytes(9), 2);
    EXPECT_EQ(get_num_presence_bytes(64), 8);
    EXPECT_EQ(get_num_presence_bytes(65), 9);
}

TEST(FstLabelTest, RoundTripsEveryInputType) {
    for (const auto input_type : {InputType::kByte1, InputType::kByte2, InputType::kByte4}) {
        std::vector<uint8_t> buffer(16, 0);
        ByteArrayDataOutput out(buffer);
        for (const int32_t label : {0, 1, 42, 255, input_type == InputType::kByte1 ? 255 : 40000}) {
            if (input_type == InputType::kByte1 && label > 255) {
                continue;
            }
            auto written = write_label(out, input_type, label);
            ASSERT_TRUE(written.has_value()) << (written ? "" : written.error().to_string());
        }

        ByteArrayDataInput in(std::span<const uint8_t>(buffer.data(), out.get_position()));
        for (const int32_t label : {0, 1, 42, 255, input_type == InputType::kByte1 ? 255 : 40000}) {
            if (input_type == InputType::kByte1 && label > 255) {
                continue;
            }
            auto read = read_label(in, input_type, VERSION_CURRENT);
            ASSERT_TRUE(read.has_value()) << (read ? "" : read.error().to_string());
            EXPECT_EQ(read.value(), label) << "inputType=" << static_cast<int>(input_type);
        }
        EXPECT_TRUE(in.eof());
    }
}

TEST(FstLabelTest, ReadsLegacyBigEndianByte2Labels) {
    // Before VERSION_LITTLE_ENDIAN, BYTE2 labels were big endian.
    const std::vector<uint8_t> bytes{0x01, 0x02};
    ByteArrayDataInput in(bytes);
    auto label = read_label(in, InputType::kByte2, VERSION_START);
    ASSERT_TRUE(label.has_value()) << (label ? "" : label.error().to_string());
    EXPECT_EQ(label.value(), 0x0102);

    // From VERSION_LITTLE_ENDIAN on, the same bytes mean the little-endian value.
    ByteArrayDataInput little_endian(bytes);
    auto little = read_label(little_endian, InputType::kByte2, VERSION_LITTLE_ENDIAN);
    ASSERT_TRUE(little.has_value());
    EXPECT_EQ(little.value(), 0x0201);
}
