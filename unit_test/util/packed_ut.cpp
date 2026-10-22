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
#include <limits>

module lucene_core:ut.packed;

import lucene_core;

using namespace lucene;

namespace {

/// Minimal [`RandomAccessInput`] over an in-memory buffer.
class ArrayRandomAccessInput final : public RandomAccessInput {
public:
    explicit ArrayRandomAccessInput(std::span<const uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] Result<uint8_t> read_byte(size_t pos) const override {
        if (pos >= bytes_.size()) {
            return err(LuceneError::io("read past end"));
        }
        return bytes_[pos];
    }

    [[nodiscard]] Result<int16_t> read_short(size_t pos) const override { return read_integer<int16_t>(pos, 2); }

    [[nodiscard]] Result<int32_t> read_int(size_t pos) const override { return read_integer<int32_t>(pos, 4); }

    [[nodiscard]] Result<int64_t> read_long(size_t pos) const override { return read_integer<int64_t>(pos, 8); }

    [[nodiscard]] VoidResult read_bytes(size_t pos, std::span<uint8_t> out) const override {
        if (pos + out.size() > bytes_.size()) {
            return err(LuceneError::io("read past end"));
        }
        for (size_t i = 0; i < out.size(); ++i) {
            out[i] = bytes_[pos + i];
        }
        return {};
    }

    [[nodiscard]] Result<size_t> length() const override { return bytes_.size(); }

private:
    template <class T>
    [[nodiscard]] Result<T> read_integer(size_t pos, size_t num_bytes) const {
        if (pos + num_bytes > bytes_.size()) {
            return err(LuceneError::io("read past end"));
        }
        uint64_t value = 0;
        for (size_t i = 0; i < num_bytes; ++i) {
            value |= static_cast<uint64_t>(bytes_[pos + i]) << (8 * i);
        }
        return static_cast<T>(value);
    }

    std::span<const uint8_t> bytes_;
};

/// Number of trailing padding bytes `DirectWriter::finish` writes.
int32_t padding_bytes_for(int32_t bits_per_value) {
    int32_t padding_bits = 0;
    if (bits_per_value > 32) {
        padding_bits = 64 - bits_per_value;
    } else if (bits_per_value > 16) {
        padding_bits = 32 - bits_per_value;
    } else if (bits_per_value > 8) {
        padding_bits = 16 - bits_per_value;
    }
    return (padding_bits + 7) / 8;
}

/// Writes `values` with `bits_per_value` and returns the raw bytes.
std::vector<uint8_t> write_packed(std::span<const int64_t> values, int32_t bits_per_value) {
    std::vector<uint8_t> buffer(values.size() * 8 + 64, 0);
    ByteArrayDataOutput output(buffer);
    auto writer = DirectWriter::get_instance(output, static_cast<int64_t>(values.size()), bits_per_value);
    EXPECT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    for (const int64_t value : values) {
        auto added = writer->add(value);
        EXPECT_TRUE(added.has_value()) << (added ? "" : added.error().to_string());
    }
    auto finished = writer->finish();
    EXPECT_TRUE(finished.has_value()) << (finished ? "" : finished.error().to_string());
    buffer.resize(output.get_position());
    return buffer;
}

std::vector<int64_t> values_for(int32_t bits_per_value, size_t count) {
    const int64_t max = PackedInts::max_value(bits_per_value);
    const bool full_range = max == std::numeric_limits<int64_t>::max();
    std::vector<int64_t> values;
    values.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        // Alternate small and large values so both ends of the range are hit.
        if (full_range) {
            values.push_back((i % 2) == 0 ? static_cast<int64_t>(i) : -static_cast<int64_t>(i));
        } else if ((i % 2) == 0) {
            values.push_back(static_cast<int64_t>(i) % (max + 1));
        } else {
            values.push_back(max - (static_cast<int64_t>(i) % (max + 1)));
        }
    }
    return values;
}

} // namespace

TEST(PackedIntsTest, BitWidthHelpers) {
    EXPECT_EQ(PackedInts::unsigned_bits_required(0), 1);
    EXPECT_EQ(PackedInts::unsigned_bits_required(1), 1);
    EXPECT_EQ(PackedInts::unsigned_bits_required(2), 2);
    EXPECT_EQ(PackedInts::unsigned_bits_required(255), 8);
    EXPECT_EQ(PackedInts::unsigned_bits_required(256), 9);
    EXPECT_EQ(PackedInts::max_value(1), 1);
    EXPECT_EQ(PackedInts::max_value(8), 255);
    EXPECT_EQ(PackedInts::max_value(32), 4294967295LL);
    EXPECT_EQ(PackedInts::max_value(64), std::numeric_limits<int64_t>::max());

    ASSERT_TRUE(PackedInts::bits_required(1000).has_value());
    EXPECT_EQ(PackedInts::bits_required(1000).value(), 10);
    auto negative = PackedInts::bits_required(-1);
    ASSERT_FALSE(negative.has_value());
    EXPECT_EQ(negative.error().kind(), ErrorKind::kIllegalArgument);

    EXPECT_TRUE(PackedInts::check_version(PackedInts::VERSION_CURRENT).has_value());
    EXPECT_FALSE(PackedInts::check_version(PackedInts::VERSION_START - 1).has_value());
    EXPECT_FALSE(PackedInts::check_version(PackedInts::VERSION_CURRENT + 1).has_value());

    ASSERT_TRUE(PackedInts::num_blocks(0, 16).has_value());
    EXPECT_EQ(PackedInts::num_blocks(0, 16).value(), 0u);
    EXPECT_EQ(PackedInts::num_blocks(17, 16).value(), 2u);
    EXPECT_FALSE(PackedInts::num_blocks(17, 0).has_value());
}

TEST(PackedIntsTest, ByteCountsAndFormatSelection) {
    EXPECT_EQ(packed_byte_count(PackedInts::VERSION_CURRENT, 0, 8), 0u);
    EXPECT_EQ(packed_byte_count(PackedInts::VERSION_CURRENT, 3, 1), 1u);
    EXPECT_EQ(packed_byte_count(PackedInts::VERSION_CURRENT, 100, 12), 150u);
    EXPECT_EQ(packed_byte_count(PackedInts::VERSION_CURRENT, 1, 64), 8u);

    ASSERT_TRUE(packed_single_block_byte_count(PackedInts::VERSION_CURRENT, 1, 16).has_value());
    EXPECT_EQ(packed_single_block_byte_count(PackedInts::VERSION_CURRENT, 1, 16).value(), 8u);
    EXPECT_EQ(packed_single_block_byte_count(PackedInts::VERSION_CURRENT, 9, 8).value(), 16u);
    EXPECT_FALSE(packed_single_block_byte_count(PackedInts::VERSION_CURRENT, 9, 12).has_value());

    // COMPACT keeps the requested width, FASTEST rounds up to a byte.
    EXPECT_EQ(fastest_format_and_bits(1000, 1, PackedInts::COMPACT).bits_per_value, 1);
    EXPECT_EQ(fastest_format_and_bits(1000, 1, PackedInts::FASTEST).bits_per_value, 8);
    EXPECT_EQ(fastest_format_and_bits(1000, 12, PackedInts::FASTEST).bits_per_value, 16);
    // 20 bits + 50% overhead is 30 bits, still below 32, so nothing rounds up.
    EXPECT_EQ(fastest_format_and_bits(1000, 20, 0.5F).bits_per_value, 20);
    EXPECT_EQ(fastest_format_and_bits(1000, 20, PackedInts::FASTEST).bits_per_value, 32);
    EXPECT_EQ(fastest_format_and_bits(1000, 40, PackedInts::COMPACT).bits_per_value, 40);
}

TEST(DirectPackedTest, BitRequirementsRoundUpToSupportedWidths) {
    // `bits_required` takes the maximum *value*, not a bit count.
    EXPECT_EQ(DirectWriter::bits_required(0).value(), 1);
    EXPECT_EQ(DirectWriter::bits_required(1).value(), 1);
    EXPECT_EQ(DirectWriter::bits_required(3).value(), 2);
    EXPECT_EQ(DirectWriter::bits_required(15).value(), 4);
    EXPECT_EQ(DirectWriter::bits_required(16).value(), 8);
    EXPECT_EQ(DirectWriter::bits_required(255).value(), 8);
    EXPECT_EQ(DirectWriter::bits_required(256).value(), 12);
    EXPECT_EQ(DirectWriter::bits_required(4096).value(), 16);
    EXPECT_EQ(DirectWriter::bits_required(1 << 20).value(), 24);
    EXPECT_EQ(DirectWriter::bits_required(std::numeric_limits<int64_t>::max()).value(), 64);
    EXPECT_FALSE(DirectWriter::bits_required(-5).has_value());
    EXPECT_EQ(DirectWriter::unsigned_bits_required(255), 8);
    EXPECT_EQ(DirectWriter::unsigned_bits_required(256), 12);
}

TEST(DirectPackedTest, RoundTripsEverySupportedBitWidth) {
    for (const int32_t bits_per_value : DirectWriter::SUPPORTED_BITS_PER_VALUE) {
        const auto values = values_for(bits_per_value, 100);
        const auto bytes = write_packed(values, bits_per_value);

        const size_t expected_size = packed_byte_count(PackedInts::VERSION_CURRENT,
                                                       static_cast<int64_t>(values.size()), bits_per_value) +
                                     static_cast<size_t>(padding_bytes_for(bits_per_value));
        EXPECT_EQ(bytes.size(), expected_size) << "bitsPerValue=" << bits_per_value;

        const ArrayRandomAccessInput input(bytes);
        auto reader = DirectReader::get_instance(input, bits_per_value);
        ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
        for (size_t i = 0; i < values.size(); ++i) {
            auto value = reader->get(i);
            ASSERT_TRUE(value.has_value()) << (value ? "" : value.error().to_string());
            EXPECT_EQ(value.value(), values[i]) << "bitsPerValue=" << bits_per_value << " index=" << i;
        }
    }
}

TEST(DirectPackedTest, RoundTripsAcrossBlockBoundaries) {
    // 64 values per 64-bit word for 1 bit per value; 512 exceeds one internal
    // buffer (64 values) several times for every width.
    for (const int32_t bits_per_value : {1, 2, 4, 8, 12, 16, 20, 28, 32, 64}) {
        for (const size_t count : {size_t{1}, size_t{63}, size_t{65}, size_t{512}}) {
            const auto values = values_for(bits_per_value, count);
            const auto bytes = write_packed(values, bits_per_value);
            const ArrayRandomAccessInput input(bytes);
            auto reader = DirectReader::get_instance(input, bits_per_value);
            ASSERT_TRUE(reader.has_value());
            for (size_t i = 0; i < count; ++i) {
                auto value = reader->get(i);
                ASSERT_TRUE(value.has_value()) << (value ? "" : value.error().to_string());
                EXPECT_EQ(value.value(), values[i])
                    << "bitsPerValue=" << bits_per_value << " count=" << count << " index=" << i;
            }
        }
    }
}

TEST(DirectPackedTest, RejectsUnsupportedBitWidthAndWrongCount) {
    std::vector<uint8_t> buffer(1024, 0);
    ByteArrayDataOutput output(buffer);
    auto writer = DirectWriter::get_instance(output, 4, 3);
    ASSERT_FALSE(writer.has_value());
    EXPECT_EQ(writer.error().kind(), ErrorKind::kIllegalArgument);

    auto short_writer = DirectWriter::get_instance(output, 4, 8);
    ASSERT_TRUE(short_writer.has_value());
    ASSERT_TRUE(short_writer->add(1).has_value());
    auto too_early = short_writer->finish();
    ASSERT_FALSE(too_early.has_value());
    EXPECT_EQ(too_early.error().kind(), ErrorKind::kIllegalState);

    // Writing past the declared count fails.
    auto exact_writer = DirectWriter::get_instance(output, 1, 8);
    ASSERT_TRUE(exact_writer.has_value());
    ASSERT_TRUE(exact_writer->add(7).has_value());
    auto overflow = exact_writer->add(8);
    ASSERT_FALSE(overflow.has_value());
    EXPECT_EQ(overflow.error().kind(), ErrorKind::kEof);

    std::vector<uint8_t> empty;
    const ArrayRandomAccessInput input(empty);
    auto reader = DirectReader::get_instance(input, 3);
    ASSERT_FALSE(reader.has_value());
}

TEST(Packed64Test, GetAndSetAcrossWordBoundaries) {
    for (const int32_t bits_per_value : {1, 4, 12, 20, 32, 64}) {
        auto array = Packed64::create(200, bits_per_value);
        ASSERT_TRUE(array.has_value()) << (array ? "" : array.error().to_string());
        EXPECT_EQ(array->size(), 200u);
        EXPECT_EQ(array->bits_per_value(), bits_per_value);

        const int64_t max = PackedInts::max_value(bits_per_value);
        std::vector<int64_t> expected(200, 0);
        for (size_t i = 0; i < 200; ++i) {
            const int64_t value =
                bits_per_value == 64 ? static_cast<int64_t>(i) * 977 : (static_cast<int64_t>(i) * 977) % (max + 1);
            expected[i] = value;
            auto set = array->set(i, value);
            ASSERT_TRUE(set.has_value()) << (set ? "" : set.error().to_string());
        }
        for (size_t i = 0; i < 200; ++i) {
            EXPECT_EQ(array->get(i), expected[i]) << "bitsPerValue=" << bits_per_value << " index=" << i;
        }

        // `fill` over a range that crosses word boundaries.
        ASSERT_TRUE(array->fill(13, 71, 0).has_value());
        for (size_t i = 0; i < 200; ++i) {
            EXPECT_EQ(array->get(i), (i >= 13 && i < 71) ? 0 : expected[i]);
        }
        array->clear();
        for (size_t i = 0; i < 200; ++i) {
            EXPECT_EQ(array->get(i), 0);
        }
    }
}

TEST(Packed64Test, RejectsOutOfRangeValues) {
    auto array = Packed64::create(4, 4);
    ASSERT_TRUE(array.has_value());
    EXPECT_FALSE(array->set(4, 0).has_value());
    auto too_large = array->set(0, 16);
    ASSERT_FALSE(too_large.has_value());
    EXPECT_EQ(too_large.error().kind(), ErrorKind::kIllegalArgument);
    EXPECT_FALSE(array->fill(2, 5, 0).has_value());
    EXPECT_FALSE(Packed64::create(4, 0).has_value());
    EXPECT_FALSE(Packed64::create(4, 65).has_value());
}

TEST(GrowableWriterTest, GrowsBitWidthOnDemand) {
    auto writer = GrowableWriter::create(1, 300, PackedInts::COMPACT);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    EXPECT_EQ(writer->bits_per_value(), 1);

    std::vector<int64_t> values;
    for (size_t i = 0; i < 300; ++i) {
        // 3 needs 2 bits, 17 needs 5 and 1000 needs 10, so the writer grows
        // three times; COMPACT keeps exactly the required width.
        const int64_t value = (i == 5) ? 3 : (i == 100) ? 17 : (i == 250) ? 1000 : 1;
        values.push_back(value);
        auto set = writer->set(i, value);
        ASSERT_TRUE(set.has_value()) << (set ? "" : set.error().to_string());
    }
    EXPECT_EQ(writer->bits_per_value(), 10);
    for (size_t i = 0; i < 300; ++i) {
        EXPECT_EQ(writer->get(i), values[i]) << "index " << i;
    }

    // A negative value forces the full 64-bit width.
    ASSERT_TRUE(writer->set(0, -1).has_value());
    EXPECT_EQ(writer->bits_per_value(), 64);
    EXPECT_EQ(writer->get(0), -1);
    for (size_t i = 1; i < 300; ++i) {
        EXPECT_EQ(writer->get(i), values[i]) << "index " << i;
    }
}

TEST(GrowableWriterTest, BulkFillAndResize) {
    auto writer = GrowableWriter::create(1, 8, PackedInts::COMPACT);
    ASSERT_TRUE(writer.has_value());

    const std::vector<int64_t> bulk{1, 2, 3, 100, 5};
    ASSERT_TRUE(writer->set_bulk(0, bulk).has_value());
    for (size_t i = 0; i < bulk.size(); ++i) {
        EXPECT_EQ(writer->get(i), bulk[i]);
    }

    ASSERT_TRUE(writer->fill(5, 8, 7).has_value());
    for (size_t i = 5; i < 8; ++i) {
        EXPECT_EQ(writer->get(i), 7);
    }

    auto grown = writer->resize(12);
    ASSERT_TRUE(grown.has_value()) << (grown ? "" : grown.error().to_string());
    EXPECT_EQ(grown->size(), 12u);
    EXPECT_EQ(grown->bits_per_value(), writer->bits_per_value());
    for (size_t i = 0; i < 8; ++i) {
        EXPECT_EQ(grown->get(i), writer->get(i)) << "index " << i;
    }
    EXPECT_EQ(grown->get(11), 0);

    auto shrunk = writer->resize(3);
    ASSERT_TRUE(shrunk.has_value());
    EXPECT_EQ(shrunk->size(), 3u);
    EXPECT_EQ(shrunk->get(3 - 1), bulk[2]);
}

TEST(PackedReaderTest, NullReaderIsAllZeros) {
    const NullPackedReader reader(5);
    EXPECT_EQ(reader.size(), 5u);
    EXPECT_EQ(reader.bits_per_value(), 0);
    EXPECT_EQ(reader.get(0), 0);
    EXPECT_EQ(reader.get(4), 0);
}
