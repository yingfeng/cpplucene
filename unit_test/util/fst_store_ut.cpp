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
#include <string>
#include <vector>

module lucene_core:ut.fst_store;

import lucene_core;

using namespace lucene;

namespace {

std::vector<int32_t> key_of(std::string_view text) {
    std::vector<int32_t> key;
    key.reserve(text.size());
    for (const char character : text) {
        key.push_back(static_cast<int32_t>(static_cast<unsigned char>(character)));
    }
    return key;
}

/// Builds a small BYTE1 FST and returns its bytes plus metadata.
struct Built {
    std::vector<uint8_t> data;
    FSTMetadata metadata;
};

Built build_fst() {
    const auto &outputs = FstByteSequenceOutputs::singleton();
    FstBuilder builder(outputs, InputType::kByte1);
    const std::vector<std::pair<std::string, std::string>> entries{{"a", "1"}, {"ab", "22"}, {"b", "3"}};
    for (const auto &[key, output] : entries) {
        auto added = builder.add(key_of(key), std::any{BytesRef::from_string(output)});
        EXPECT_TRUE(added.has_value());
    }
    auto result = builder.finish();
    EXPECT_TRUE(result.has_value());
    Built built;
    built.data = std::move(result->data);
    built.metadata = result->metadata;
    return built;
}

std::string lookup(FST &fst, std::string_view key) {
    auto value = FstUtil::get_from_bytes(fst, BytesRef::from_string(key));
    if (!value.has_value() || !value->has_value()) {
        return "<miss>";
    }
    const auto &bytes = std::any_cast<const BytesRef &>(value->value());
    return std::string(reinterpret_cast<const char *>(bytes.as_slice().data()), bytes.as_slice().size());
}

} // namespace

TEST(FstStoreTest, OnHeapStoreReadsBackTheFst) {
    auto built = build_fst();
    const OnHeapFstStore store(built.data);

    EXPECT_EQ(store.num_bytes(), static_cast<int64_t>(built.data.size()));
    EXPECT_EQ(store.ram_bytes_used(), static_cast<int64_t>(built.data.size()));
    EXPECT_TRUE(store.contains(0));
    EXPECT_FALSE(store.contains(static_cast<int64_t>(built.data.size())));

    FST fst(built.metadata, store.make_reader());
    EXPECT_EQ(lookup(fst, "a"), "1");
    EXPECT_EQ(lookup(fst, "ab"), "22");
    EXPECT_EQ(lookup(fst, "b"), "3");
    EXPECT_EQ(lookup(fst, "abc"), "<miss>");
}

TEST(FstStoreTest, ReadersKeepTheBufferAlive) {
    auto built = build_fst();
    std::unique_ptr<FstBytesReader> reader;
    {
        OnHeapFstStore store(built.data);
        reader = store.make_reader();
        // `store` dies here; the reader shares ownership of the bytes.
    }
    auto byte = reader->read_byte();
    ASSERT_TRUE(byte.has_value());
    EXPECT_EQ(byte.value(), built.data[0]);
    reader->set_position(0);
    EXPECT_EQ(reader->get_position(), 0);
}

TEST(FstStoreTest, ReaderReportsEofPastTheEnd) {
    OwnedBytesReader reader(std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{1, 2, 3}), 0, 3);
    EXPECT_TRUE(reader.read_byte().has_value());
    EXPECT_TRUE(reader.read_byte().has_value());
    EXPECT_TRUE(reader.read_byte().has_value());
    EXPECT_FALSE(reader.read_byte().has_value());

    reader.set_position(2);
    std::vector<uint8_t> too_much(2);
    EXPECT_FALSE(reader.read_bytes(too_much).has_value());
}

TEST(FstStoreTest, ReadFromDataInputTakesExactlyNumBytes) {
    auto built = build_fst();
    std::vector<uint8_t> framed{0xAA, 0xBB};
    framed.insert(framed.end(), built.data.begin(), built.data.end());
    framed.insert(framed.end(), {0xCC});

    ByteArrayDataInput input(framed);
    ASSERT_TRUE(input.skip_bytes(2).has_value());
    auto store = OnHeapFstStore::read_from(input, static_cast<int64_t>(built.data.size()));
    ASSERT_TRUE(store.has_value()) << (store ? "" : store.error().to_string());
    EXPECT_EQ(store->num_bytes(), static_cast<int64_t>(built.data.size()));

    FST fst(built.metadata, store->make_reader());
    EXPECT_EQ(lookup(fst, "ab"), "22");

    // A negative length is rejected instead of allocating nonsense.
    ByteArrayDataInput negative(framed);
    EXPECT_FALSE(OnHeapFstStore::read_from(negative, -1).has_value());
}

TEST(FstStoreTest, WriteToCopiesTheBytes) {
    auto built = build_fst();
    const OnHeapFstStore store(built.data);

    GrowableByteArrayDataOutput out;
    ASSERT_TRUE(store.write_to(out).has_value());
    EXPECT_EQ(out.bytes(), built.data);

    // Copying back in gives the same FST.
    ByteArrayDataInput input(out.bytes());
    auto copy = OnHeapFstStore::read_from(input, store.num_bytes());
    ASSERT_TRUE(copy.has_value());
    FST fst(built.metadata, copy->make_reader());
    EXPECT_EQ(lookup(fst, "b"), "3");
}

TEST(FstStoreTest, OffHeapStoreReadsASlice) {
    auto built = build_fst();
    std::vector<uint8_t> padded(built.data.size() + 16, 0xEE);
    std::copy(built.data.begin(), built.data.end(), padded.begin() + 8);

    const OffHeapFstStore store(padded, 8, built.data.size());
    EXPECT_EQ(store.num_bytes(), static_cast<int64_t>(built.data.size()));
    EXPECT_EQ(store.ram_bytes_used(), 0);

    FST fst(built.metadata, store.make_reader());
    EXPECT_EQ(lookup(fst, "a"), "1");
    EXPECT_EQ(lookup(fst, "ab"), "22");

    // The bytes already live in the index file, so writing them out again is
    // rejected rather than silently producing a duplicate.
    GrowableByteArrayDataOutput out;
    EXPECT_FALSE(store.write_to(out).has_value());
}
