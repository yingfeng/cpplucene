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

module lucene_core:ut.bkd_point_writer;

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

TEST(PointWritersTest, DispatchesToTheHeapWriter) {
    const BKDConfig config = one_dim_config();
    HeapPointWriter heap(config, 2);
    const PointWriterVariant writer{&heap};

    EXPECT_TRUE(PointWriters::is_on_heap(writer));
    EXPECT_EQ(PointWriters::heap_writer(writer), &heap);
    EXPECT_EQ(PointWriters::offline_writer(writer), nullptr);

    ASSERT_TRUE(PointWriters::append_bytes(writer, pack_one(11), 3).has_value());
    ASSERT_TRUE(PointWriters::append_bytes(writer, pack_one(12), 4).has_value());
    EXPECT_EQ(PointWriters::count(writer), 2u);
}

TEST(PointWritersTest, DispatchesToTheOfflineWriter) {
    ByteBuffersDirectory dir;
    const BKDConfig config = one_dim_config();
    auto offline = OfflinePointWriter::create(config, dir, "points", "dispatch", 2);
    ASSERT_TRUE(offline.has_value());

    const PointWriterVariant writer{&offline.value()};
    EXPECT_FALSE(PointWriters::is_on_heap(writer));
    EXPECT_EQ(PointWriters::heap_writer(writer), nullptr);
    EXPECT_EQ(PointWriters::offline_writer(writer), &offline.value());

    ASSERT_TRUE(PointWriters::append_bytes(writer, pack_one(1), 1).has_value());
    ASSERT_TRUE(PointWriters::append_bytes(writer, pack_one(2), 2).has_value());
    EXPECT_EQ(PointWriters::count(writer), 2u);
    ASSERT_TRUE(offline->close().has_value());

    auto reader = offline->make_reader(dir, 0, 2, std::vector<uint8_t>(config.bytes_per_doc()));
    ASSERT_TRUE(reader.has_value());
    ASSERT_TRUE(reader->next().value());
    EXPECT_EQ(reader->point_value().doc_id(), 1);
}

TEST(PointWritersTest, RejectsNullWriters) {
    const PointWriterVariant writer{static_cast<HeapPointWriter *>(nullptr)};
    EXPECT_FALSE(PointWriters::is_on_heap(writer));
    EXPECT_EQ(PointWriters::count(writer), 0u);
    EXPECT_FALSE(PointWriters::append_bytes(writer, pack_one(1), 1).has_value());
    EXPECT_FALSE(PointWriters::append_packed(writer, pack_one(1)).has_value());
}
