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
#include <vector>

module lucene_core:ut.packed_ints_io;

import lucene_core;

using namespace lucene;

namespace {

/// A value that fits in `bits` bits, spread over the whole range.
int64_t sample_value(int32_t bits, size_t index) {
    const uint64_t mask = bits >= 64 ? std::numeric_limits<uint64_t>::max() : (uint64_t{1} << bits) - 1;
    return static_cast<int64_t>((index * 2654435761ULL + 12345ULL) & mask);
}

std::vector<int64_t> sample_values(int32_t bits, size_t count) {
    std::vector<int64_t> values(count);
    for (size_t i = 0; i < count; ++i) {
        values[i] = sample_value(bits, i);
    }
    return values;
}

} // namespace

TEST(PackedIntsIoTest, RoundTripsEveryWidth) {
    for (int32_t bits = 1; bits <= 64; ++bits) {
        const std::vector<int64_t> values = sample_values(bits, 97);
        std::vector<uint8_t> storage(packed_stream_byte_count(values.size(), bits) + 8);
        ByteArrayDataOutput out(storage);

        auto writer = PackedInts::get_writer(PackedFormat::kPacked, PackedInts::VERSION_CURRENT, bits,
                                             values.size(), out);
        ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
        for (const int64_t value : values) {
            ASSERT_TRUE(writer.value()->add(value).has_value());
        }
        ASSERT_TRUE(writer.value()->finish().has_value());
        const size_t written = out.get_position();
        EXPECT_EQ(written, packed_stream_byte_count(values.size(), bits));

        ByteArrayDataInput in(std::span<const uint8_t>(storage.data(), written));
        auto reader = PackedInts::get_reader(PackedFormat::kPacked, PackedInts::VERSION_CURRENT, bits,
                                             values.size(), in);
        ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
        ASSERT_EQ(reader.value()->size(), values.size());
        EXPECT_EQ(reader.value()->bits_per_value(), bits);
        for (size_t i = 0; i < values.size(); ++i) {
            EXPECT_EQ(reader.value()->get(i), values[i]) << "bits=" << bits << " i=" << i;
        }
        EXPECT_GT(reader.value()->ram_bytes_used(), 0);
    }
}

TEST(PackedIntsIoTest, RejectsAValueThatDoesNotFit) {
    std::vector<uint8_t> storage(64);
    ByteArrayDataOutput out(storage);
    auto writer = PackedInts::get_writer(PackedFormat::kPacked, PackedInts::VERSION_CURRENT, 4, 3, out);
    ASSERT_TRUE(writer.has_value());

    EXPECT_FALSE(writer.value()->add(16).has_value());
    EXPECT_FALSE(writer.value()->add(-1).has_value());
    ASSERT_TRUE(writer.value()->add(15).has_value());
    ASSERT_EQ(writer.value()->ordinal(), 1);

    // Too few values for the declared count.
    auto finished = writer.value()->finish();
    ASSERT_FALSE(finished.has_value());
    EXPECT_NE(finished.error().to_string().find("expected 3"), std::string::npos);
}

TEST(PackedIntsIoTest, TheIteratorBatchesAndResets) {
    constexpr int32_t bits = 5;
    const std::vector<int64_t> values = sample_values(bits, 40);
    std::vector<uint8_t> storage(packed_stream_byte_count(values.size(), bits) + 8);
    ByteArrayDataOutput out(storage);
    auto writer = PackedInts::get_writer(PackedFormat::kPacked, PackedInts::VERSION_CURRENT, bits, values.size(),
                                        out);
    ASSERT_TRUE(writer.has_value());
    for (const int64_t value : values) {
        ASSERT_TRUE(writer.value()->add(value).has_value());
    }
    ASSERT_TRUE(writer.value()->finish().has_value());

    ByteArrayDataInput in(std::span<const uint8_t>(storage.data(), out.get_position()));
    auto iterator = PackedInts::get_reader_iterator(PackedFormat::kPacked, PackedInts::VERSION_CURRENT, bits,
                                                    values.size(), in);
    ASSERT_TRUE(iterator.has_value()) << (iterator ? "" : iterator.error().to_string());
    EXPECT_EQ(iterator.value()->size(), values.size());

    ASSERT_EQ(iterator.value()->next().value(), values[0]);
    EXPECT_EQ(iterator.value()->ordinal(), 1);
    const auto batch = iterator.value()->next(size_t{10});
    ASSERT_TRUE(batch.has_value());
    for (size_t i = 0; i < batch.value().size(); ++i) {
        EXPECT_EQ(batch.value()[i], values[1 + i]);
    }
    EXPECT_EQ(iterator.value()->ordinal(), 11);

    // Asking for more than is left is an error, not a short read.
    EXPECT_FALSE(iterator.value()->next(size_t{100}).has_value());

    const auto rest = iterator.value()->next(values.size() - 11);
    ASSERT_TRUE(rest.has_value());
    EXPECT_EQ(rest.value().size(), values.size() - 11);
    EXPECT_TRUE(iterator.value()->is_exhausted());
    EXPECT_FALSE(iterator.value()->next().has_value());

    iterator.value()->reset();
    EXPECT_EQ(iterator.value()->ordinal(), 0);
    EXPECT_EQ(iterator.value()->next().value(), values[0]);
}

TEST(PackedIntsIoTest, RejectsAnUnknownVersionAndTheSingleBlockLayout) {
    std::vector<uint8_t> storage(64);
    ByteArrayDataOutput out(storage);
    EXPECT_FALSE(PackedInts::get_writer(PackedFormat::kPacked, 99, 4, 1, out).has_value());

    ByteArrayDataInput in(storage);
    auto unsupported = PackedInts::get_reader(PackedFormat::kPackedSingleBlock, PackedInts::VERSION_CURRENT, 4, 1,
                                              in);
    ASSERT_FALSE(unsupported.has_value());
    EXPECT_NE(unsupported.error().to_string().find("single block"), std::string::npos);
}

TEST(PagedMutableTest, StoresValuesAcrossPages) {
    // 32 bits per value puts 256 values on a page, so 1000 values span four of them.
    auto array = PagedMutable::create(1000, 32);
    ASSERT_TRUE(array.has_value()) << (array ? "" : array.error().to_string());
    ASSERT_EQ(array.value().size(), 1000U);
    EXPECT_EQ(array.value().bits_per_value(), 32);
    ASSERT_EQ(array.value().page_size(), 256U);

    for (size_t i = 0; i < 1000; ++i) {
        ASSERT_TRUE(array.value().set(i, static_cast<int64_t>(i)).has_value());
    }
    EXPECT_EQ(array.value().get(0), 0);
    EXPECT_EQ(array.value().get(255), 255);
    // The value right after the first page boundary.
    EXPECT_EQ(array.value().get(array.value().page_size()), static_cast<int64_t>(array.value().page_size()));
    EXPECT_EQ(array.value().get(999), 999);

    // Out of range access reports instead of reading garbage.
    EXPECT_EQ(array.value().get(1000), std::numeric_limits<int64_t>::min());
    EXPECT_FALSE(array.value().set(1000, 1).has_value());

    ASSERT_TRUE(array.value().fill(0, 10, 7).has_value());
    EXPECT_EQ(array.value().get(9), 7);
    EXPECT_EQ(array.value().get(10), 10);
    EXPECT_FALSE(array.value().fill(0, 2000, 1).has_value());
    EXPECT_GT(array.value().ram_bytes_used(), 0);
}

TEST(PagedMutableTest, ResizesKeepingTheValues) {
    auto array = PagedMutable::create(4, 6);
    ASSERT_TRUE(array.has_value());
    for (size_t i = 0; i < 4; ++i) {
        ASSERT_TRUE(array.value().set(i, static_cast<int64_t>(i * 10)).has_value());
    }

    auto grown = array.value().resize(8);
    ASSERT_TRUE(grown.has_value());
    EXPECT_EQ(grown.value().size(), 8U);
    EXPECT_EQ(grown.value().get(3), 30);
    EXPECT_EQ(grown.value().get(7), 0);

    auto shrunk = array.value().resize(2);
    ASSERT_TRUE(shrunk.has_value());
    EXPECT_EQ(shrunk.value().size(), 2U);
    EXPECT_EQ(shrunk.value().get(1), 10);
}

TEST(PagedGrowableWriterTest, WidensWhenAValueDoesNotFit) {
    auto writer = PagedGrowableWriter::create(4, 2);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    EXPECT_EQ(writer.value().bits_per_value(), 2);

    ASSERT_TRUE(writer.value().set(0, 3).has_value());
    EXPECT_EQ(writer.value().bits_per_value(), 2);

    // 100 needs 7 bits: the array re-pages itself and keeps what it had.
    ASSERT_TRUE(writer.value().set(1, 100).has_value());
    EXPECT_EQ(writer.value().bits_per_value(), 7);
    EXPECT_EQ(writer.value().get(0), 3);
    EXPECT_EQ(writer.value().get(1), 100);

    ASSERT_TRUE(writer.value().set(2, 1).has_value());
    EXPECT_EQ(writer.value().bits_per_value(), 7);

    // A negative value is not a packed value.
    EXPECT_FALSE(writer.value().set(3, -1).has_value());
    EXPECT_GT(writer.value().ram_bytes_used(), 0);

    auto resized = writer.value().resize(6);
    ASSERT_TRUE(resized.has_value());
    EXPECT_EQ(resized.value().size(), 6U);
    EXPECT_EQ(resized.value().get(1), 100);
    EXPECT_EQ(resized.value().bits_per_value(), 7);
}

TEST(LongBitSetTest, SetsClearsAndCounts) {
    auto bits = LongBitSet::create(130);
    ASSERT_TRUE(bits.has_value()) << (bits ? "" : bits.error().to_string());
    EXPECT_EQ(bits.value().length(), 130);
    EXPECT_EQ(bits.value().cardinality(), 0);
    EXPECT_TRUE(bits.value().is_empty());

    ASSERT_TRUE(bits.value().set(0).has_value());
    ASSERT_TRUE(bits.value().set(64).has_value());
    ASSERT_TRUE(bits.value().set(129).has_value());
    EXPECT_EQ(bits.value().cardinality(), 3);
    EXPECT_TRUE(bits.value().get(0).value());
    EXPECT_FALSE(bits.value().get(1).value());
    EXPECT_TRUE(bits.value().get(129).value());

    // Out of range access is an error, not a silent zero.
    EXPECT_FALSE(bits.value().get(130).has_value());
    EXPECT_FALSE(bits.value().set(-1).has_value());

    ASSERT_TRUE(bits.value().clear(64).has_value());
    EXPECT_EQ(bits.value().cardinality(), 2);
    ASSERT_TRUE(bits.value().flip(64).has_value());
    EXPECT_TRUE(bits.value().get(64).value());
    ASSERT_TRUE(bits.value().flip(64).has_value());
    EXPECT_FALSE(bits.value().get(64).value());

    EXPECT_EQ(bits.value().next_set_bit(0), 0);
    EXPECT_EQ(bits.value().next_set_bit(1), 129);
    EXPECT_EQ(bits.value().next_set_bit(130), -1);
    EXPECT_EQ(bits.value().prev_set_bit(129), 129);
    EXPECT_EQ(bits.value().prev_set_bit(128), 0);
    EXPECT_EQ(bits.value().prev_set_bit(0), 0);
    EXPECT_GT(bits.value().ram_bytes_used(), 0);
}

TEST(LongBitSetTest, RangesAndBulkOperations) {
    auto left = LongBitSet::create(100);
    auto right = LongBitSet::create(100);
    ASSERT_TRUE(left.has_value());
    ASSERT_TRUE(right.has_value());

    ASSERT_TRUE(left.value().set_range(10, 20).has_value());
    EXPECT_EQ(left.value().cardinality(), 10);
    ASSERT_TRUE(left.value().clear_range(15, 20).has_value());
    EXPECT_EQ(left.value().cardinality(), 5);
    EXPECT_FALSE(left.value().clear_range(0, 200).has_value());

    ASSERT_TRUE(right.value().set_range(18, 30).has_value());
    ASSERT_TRUE(left.value().or_with(right.value()));
    EXPECT_EQ(left.value().cardinality(), 17);  // 10..14 plus 18..29

    ASSERT_TRUE(left.value().and_with(right.value()));
    EXPECT_EQ(left.value().cardinality(), 12);  // 18..29
    EXPECT_TRUE(left.value().get(18).value());
    EXPECT_FALSE(left.value().get(17).value());

    ASSERT_TRUE(left.value().xor_with(right.value()));
    EXPECT_EQ(left.value().cardinality(), 0);  // the two sets are equal by now

    ASSERT_TRUE(left.value().set_all().has_value());
    EXPECT_EQ(left.value().cardinality(), 100);
    EXPECT_EQ(left.value().next_set_bit(99), 99);
    left.value().clear_all();
    EXPECT_EQ(left.value().cardinality(), 0);

    // Two sets of different lengths cannot be combined.
    auto smaller = LongBitSet::create(50);
    ASSERT_TRUE(smaller.has_value());
    EXPECT_FALSE(left.value().or_with(smaller.value()).has_value());
}
