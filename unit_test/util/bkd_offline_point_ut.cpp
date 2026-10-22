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

module lucene_core:ut.bkd_offline_point;

import lucene_core;

using namespace lucene;

namespace {

BKDConfig one_dim_config() {
    auto config = BKDConfig::create(1, 1, 4, 512);
    EXPECT_TRUE(config.has_value());
    return config.value();
}

std::vector<uint8_t> pack_one(int32_t value) {
    std::vector<uint8_t> packed(4, 0);
    BitUtil::set_i32_be(packed, 0, value);
    return packed;
}

} // namespace

TEST(OfflinePointWriterTest, WritesPointsAndAppendsFooter) {
    ByteBuffersDirectory dir;
    const BKDConfig config = one_dim_config();

    auto writer = OfflinePointWriter::create(config, dir, "points", "test", 5);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    for (int32_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(writer->append_bytes(pack_one(i * 10), 100 + i).has_value());
    }
    EXPECT_EQ(writer->count(), 5u);
    ASSERT_TRUE(writer->close().has_value());
    EXPECT_TRUE(writer->closed());

    auto length = dir.file_length(writer->name());
    ASSERT_TRUE(length.has_value());
    EXPECT_EQ(length.value(), 5 * config.bytes_per_doc() + CodecUtil::footer_length());

    // A second close is a no-op.
    EXPECT_TRUE(writer->close().has_value());
}

TEST(OfflinePointReaderTest, ReadsTheWholeFile) {
    ByteBuffersDirectory dir;
    const BKDConfig config = one_dim_config();

    auto writer = OfflinePointWriter::create(config, dir, "points", "test", 5);
    ASSERT_TRUE(writer.has_value());
    for (int32_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(writer->append_bytes(pack_one(i * 10), 100 + i).has_value());
    }
    ASSERT_TRUE(writer->close().has_value());

    auto reader = writer->make_reader(dir, 0, 5, std::vector<uint8_t>(config.bytes_per_doc()));
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());

    for (int32_t i = 0; i < 5; ++i) {
        auto more = reader->next();
        ASSERT_TRUE(more.has_value());
        ASSERT_TRUE(more.value()) << "point " << i;
        EXPECT_EQ(reader->point_value().doc_id(), 100 + i);
        EXPECT_EQ(BitUtil::get_i32_be(reader->point_value().packed_value(), 0), i * 10);
    }
    auto done = reader->next();
    ASSERT_TRUE(done.has_value());
    EXPECT_FALSE(done.value());
    EXPECT_TRUE(reader->close().has_value());
}

TEST(OfflinePointReaderTest, ReadsASubRangeAcrossBufferRefills) {
    ByteBuffersDirectory dir;
    const BKDConfig config = one_dim_config();

    auto writer = OfflinePointWriter::create(config, dir, "points", "test", 8);
    ASSERT_TRUE(writer.has_value());
    for (int32_t i = 0; i < 8; ++i) {
        ASSERT_TRUE(writer->append_bytes(pack_one(i), i + 1).has_value());
    }
    ASSERT_TRUE(writer->close().has_value());

    // A buffer holding a single point forces one read per point.
    auto reader = writer->make_reader(dir, 3, 4, std::vector<uint8_t>(config.bytes_per_doc()));
    ASSERT_TRUE(reader.has_value());
    EXPECT_EQ(reader->remaining(), 4u);

    for (int32_t i = 3; i < 7; ++i) {
        auto more = reader->next();
        ASSERT_TRUE(more.has_value());
        ASSERT_TRUE(more.value()) << "point " << i;
        EXPECT_EQ(reader->point_value().doc_id(), i + 1);
    }
    auto done = reader->next();
    ASSERT_TRUE(done.has_value());
    EXPECT_FALSE(done.value());

    // A buffer holding several points is used the same way.
    auto wide = writer->make_reader(dir, 1, 3, std::vector<uint8_t>(config.bytes_per_doc() * 3));
    ASSERT_TRUE(wide.has_value());
    std::vector<int32_t> docs;
    while (true) {
        auto more = wide->next();
        ASSERT_TRUE(more.has_value());
        if (!more.value()) {
            break;
        }
        docs.push_back(wide->point_value().doc_id());
    }
    EXPECT_EQ(docs, (std::vector<int32_t>{2, 3, 4}));
}

TEST(OfflinePointReaderTest, RejectsSlicesBeyondTheFile) {
    ByteBuffersDirectory dir;
    const BKDConfig config = one_dim_config();

    auto writer = OfflinePointWriter::create(config, dir, "points", "test", 2);
    ASSERT_TRUE(writer.has_value());
    for (int32_t i = 0; i < 2; ++i) {
        ASSERT_TRUE(writer->append_bytes(pack_one(i), i).has_value());
    }
    ASSERT_TRUE(writer->close().has_value());

    EXPECT_FALSE(writer->make_reader(dir, 1, 5, std::vector<uint8_t>(config.bytes_per_doc())).has_value());
    // A buffer that cannot hold even one point is rejected too.
    EXPECT_FALSE(writer->make_reader(dir, 0, 2, std::vector<uint8_t>(2)).has_value());
}

TEST(OfflinePointWriterTest, DestroyRemovesTheFile) {
    ByteBuffersDirectory dir;
    const BKDConfig config = one_dim_config();

    auto writer = OfflinePointWriter::create(config, dir, "points", "test", 0);
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer->append_bytes(pack_one(1), 1).has_value());
    ASSERT_TRUE(writer->close().has_value());

    const std::string name = writer->name();
    ASSERT_TRUE(dir.file_length(name).has_value());
    ASSERT_TRUE(writer->destroy(dir).has_value());
    EXPECT_FALSE(dir.file_length(name).has_value());
}

TEST(OfflinePointWriterTest, AppendPackedKeepsDocId) {
    ByteBuffersDirectory dir;
    const BKDConfig config = one_dim_config();

    auto writer = OfflinePointWriter::create(config, dir, "points", "packed", 1);
    ASSERT_TRUE(writer.has_value());

    std::vector<uint8_t> record(pack_one(42));
    record.resize(config.bytes_per_doc(), 0);
    BitUtil::set_i32_be(record, config.packed_bytes_length(), 9);
    ASSERT_TRUE(writer->append_packed(record).has_value());
    ASSERT_TRUE(writer->close().has_value());

    auto reader = writer->make_reader(dir, 0, 1, std::vector<uint8_t>(config.bytes_per_doc()));
    ASSERT_TRUE(reader.has_value());
    ASSERT_TRUE(reader->next().value());
    EXPECT_EQ(reader->point_value().doc_id(), 9);
    EXPECT_EQ(BitUtil::get_i32_be(reader->point_value().packed_value(), 0), 42);
}
