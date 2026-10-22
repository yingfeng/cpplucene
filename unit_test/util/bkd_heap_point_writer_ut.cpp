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

module lucene_core:ut.bkd_heap_point_writer;

import lucene_core;

using namespace lucene;

namespace {

BKDConfig make_config() {
    auto config = BKDConfig::create(2, 1, 4, 512);
    EXPECT_TRUE(config.has_value());
    return config.value();
}

std::vector<uint8_t> pack(std::initializer_list<int32_t> dimensions) {
    std::vector<uint8_t> packed(dimensions.size() * 4, 0);
    size_t pos = 0;
    for (const int32_t value : dimensions) {
        BitUtil::set_i32_be(packed, pos, value);
        pos += 4;
    }
    return packed;
}

} // namespace

TEST(HeapPointWriterTest, AppendsAndReportsCount) {
    const BKDConfig config = make_config();
    HeapPointWriter writer(config, 4);
    EXPECT_EQ(writer.count(), 0u);
    EXPECT_EQ(writer.capacity(), 4u);
    EXPECT_FALSE(writer.closed());

    ASSERT_TRUE(writer.append_bytes(pack({1, 10}), 7).has_value());
    ASSERT_TRUE(writer.append_bytes(pack({2, 20}), 8).has_value());
    EXPECT_EQ(writer.count(), 2u);

    ASSERT_TRUE(writer.close().has_value());
    EXPECT_TRUE(writer.closed());
}

TEST(HeapPointWriterTest, ReadsBackValuesAndDocIds) {
    const BKDConfig config = make_config();
    HeapPointWriter writer(config, 2);
    ASSERT_TRUE(writer.append_bytes(pack({5, 6}), 42).has_value());
    ASSERT_TRUE(writer.close().has_value());

    const auto &value = writer.packed_value(0);
    const auto packed = value.packed_value();
    ASSERT_EQ(packed.size(), config.packed_bytes_length());
    EXPECT_EQ(BitUtil::get_i32_be(packed, 0), 5);
    EXPECT_EQ(BitUtil::get_i32_be(packed, 4), 6);
    EXPECT_EQ(value.doc_id(), 42);

    const auto record = value.packed_value_doc_id_bytes();
    ASSERT_EQ(record.size(), config.bytes_per_doc());
    EXPECT_EQ(BitUtil::get_i32_be(record, config.packed_bytes_length()), 42);

    EXPECT_EQ(writer.byte_at(0, 3), 5);
    EXPECT_EQ(writer.byte_at(0, 7), 6);

    std::vector<uint8_t> dim(4);
    ASSERT_TRUE(writer.copy_dim(0, 4, dim, 0).has_value());
    EXPECT_EQ(BitUtil::get_i32_be(dim, 0), 6);

    std::vector<uint8_t> tail(config.bytes_per_doc() - config.packed_index_bytes_length());
    ASSERT_TRUE(writer.copy_data_dims_and_doc(0, tail, 0).has_value());
    // The tail is dimension 1 plus the doc id.
    EXPECT_EQ(BitUtil::get_i32_be(tail, 0), 6);
    EXPECT_EQ(BitUtil::get_i32_be(tail, 4), 42);
}

TEST(HeapPointWriterTest, ComparesDimensionsAndRecords) {
    const BKDConfig config = make_config();
    HeapPointWriter writer(config, 3);
    ASSERT_TRUE(writer.append_bytes(pack({1, 10}), 1).has_value());
    ASSERT_TRUE(writer.append_bytes(pack({1, 11}), 2).has_value());
    ASSERT_TRUE(writer.append_bytes(pack({2, 10}), 3).has_value());
    ASSERT_TRUE(writer.close().has_value());

    // Dimension 0: 1 == 1 < 2.
    EXPECT_EQ(writer.compare_dim(0, 1, 0), 0);
    EXPECT_LT(writer.compare_dim(0, 2, 0), 0);
    EXPECT_GT(writer.compare_dim(2, 0, 0), 0);

    // Dimension 1: 10 < 11.
    EXPECT_LT(writer.compare_dim(0, 1, 4), 0);

    // The tail starts at dimension 1, so points 0 and 1 differ there.
    EXPECT_LT(writer.compare_data_dims_and_doc(0, 1), 0);
    EXPECT_EQ(writer.compare_data_dims_and_doc(0, 0), 0);

    std::vector<uint8_t> scratch(config.bytes_per_doc() - config.packed_index_bytes_length());
    BitUtil::set_i32_be(scratch, 0, 10);
    BitUtil::set_i32_be(scratch, 4, 1);
    EXPECT_EQ(writer.compare_data_dims_and_doc_with(0, scratch, 0), 0);

    std::vector<uint8_t> dim_scratch(4);
    BitUtil::set_i32_be(dim_scratch, 0, 1);
    EXPECT_EQ(writer.compare_dim_with_scratch(0, dim_scratch, 0, 0), 0);
    BitUtil::set_i32_be(dim_scratch, 0, 3);
    EXPECT_GT(writer.compare_dim_with_scratch(0, dim_scratch, 0, 0), 0);
}

TEST(HeapPointWriterTest, ComputesCardinality) {
    const BKDConfig config = make_config();
    HeapPointWriter writer(config, 3);
    ASSERT_TRUE(writer.append_bytes(pack({1, 10}), 1).has_value());
    ASSERT_TRUE(writer.append_bytes(pack({1, 10}), 2).has_value());
    ASSERT_TRUE(writer.append_bytes(pack({2, 10}), 3).has_value());
    ASSERT_TRUE(writer.close().has_value());

    const std::vector<size_t> compare_everything{0, 0};
    EXPECT_EQ(writer.compute_cardinality(0, 3, compare_everything), 2u);

    // Skipping all four bytes of dimension 0 makes the first two points
    // indistinguishable, and the third differs only in the skipped dimension.
    const std::vector<size_t> skip_dim0{4, 0};
    EXPECT_EQ(writer.compute_cardinality(0, 3, skip_dim0), 1u);

    // A single point is always cardinality 1.
    EXPECT_EQ(writer.compute_cardinality(0, 1, compare_everything), 1u);
}

TEST(HeapPointWriterTest, SwapsPoints) {
    const BKDConfig config = make_config();
    HeapPointWriter writer(config, 2);
    ASSERT_TRUE(writer.append_bytes(pack({1, 10}), 1).has_value());
    ASSERT_TRUE(writer.append_bytes(pack({2, 20}), 2).has_value());
    ASSERT_TRUE(writer.close().has_value());

    writer.swap(0, 1);
    EXPECT_EQ(writer.packed_value(0).doc_id(), 2);
    EXPECT_EQ(writer.packed_value(1).doc_id(), 1);
    EXPECT_EQ(BitUtil::get_i32_be(writer.packed_value(0).packed_value(), 0), 2);

    writer.swap(1, 0);
    EXPECT_EQ(writer.packed_value(0).doc_id(), 1);
}

TEST(HeapPointReaderTest, WalksTheRequestedSlice) {
    const BKDConfig config = make_config();
    HeapPointWriter writer(config, 4);
    for (int32_t i = 0; i < 4; ++i) {
        ASSERT_TRUE(writer.append_bytes(pack({i, i * 10}), 100 + i).has_value());
    }
    ASSERT_TRUE(writer.close().has_value());

    auto reader = writer.make_reader(1, 2);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());

    ASSERT_TRUE(reader->next());
    EXPECT_EQ(reader->doc_id(), 101);
    EXPECT_EQ(BitUtil::get_i32_be(reader->packed_value(), 0), 1);

    ASSERT_TRUE(reader->next());
    EXPECT_EQ(reader->doc_id(), 102);
    EXPECT_EQ(BitUtil::get_i32_be(reader->packed_value(), 4), 20);

    EXPECT_FALSE(reader->next());
    EXPECT_TRUE(reader->exhausted());

    // The block moved into the reader, so the writer no longer exposes points.
    EXPECT_EQ(writer.count(), 4u);
}

TEST(HeapPointReaderTest, AppendPackedKeepsTheRecordVerbatim) {
    const BKDConfig config = make_config();
    HeapPointWriter writer(config, 1);

    std::vector<uint8_t> record(pack({9, 8}));
    record.resize(config.bytes_per_doc(), 0);
    BitUtil::set_i32_be(record, config.packed_bytes_length(), 77);
    ASSERT_TRUE(writer.append_packed(record).has_value());
    ASSERT_TRUE(writer.close().has_value());

    EXPECT_EQ(writer.packed_value(0).doc_id(), 77);
    EXPECT_EQ(BitUtil::get_i32_be(writer.packed_value(0).packed_value(), 0), 9);
}
