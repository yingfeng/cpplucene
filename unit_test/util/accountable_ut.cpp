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

module lucene_core:ut.accountable;

import lucene_core;

using namespace lucene;

namespace {

/// An object with one child, to exercise `ram_bytes_used_deep`.
class Parent final : public Accountable {
public:
    explicit Parent(std::unique_ptr<Accountable> child = nullptr) : child_(std::move(child)) {}

    [[nodiscard]] int64_t ram_bytes_used() const override { return 100; }

    [[nodiscard]] std::vector<const Accountable *> get_child_resources() const override {
        if (child_ == nullptr) {
            return {};
        }
        return {child_.get()};
    }

private:
    std::unique_ptr<Accountable> child_;
};

} // namespace

TEST(AccountableTest, AddsUpTheChildResources) {
    Parent leaf;
    EXPECT_EQ(leaf.ram_bytes_used(), 100);
    EXPECT_TRUE(leaf.get_child_resources().empty());
    EXPECT_EQ(leaf.ram_bytes_used_deep(), 100);

    Parent root(std::make_unique<Parent>(std::make_unique<Parent>()));
    EXPECT_EQ(root.ram_bytes_used_deep(), 300);
}

TEST(AccountableTest, FixedBitSetReportsItsWords) {
    FixedBitSet empty;
    EXPECT_EQ(empty.ram_bytes_used(), 64);

    FixedBitSet one_word(64);
    // 8 bytes of words plus the fixed overhead.
    EXPECT_EQ(one_word.ram_bytes_used(), 72);

    FixedBitSet many(1024);
    EXPECT_EQ(many.ram_bytes_used(), 64 + 128);
    EXPECT_GE(many.ram_bytes_used(), one_word.ram_bytes_used());
}

TEST(AccountableTest, ByteBlockPoolReportsItsBlocks) {
    ByteBlockPool pool;
    EXPECT_GE(pool.ram_bytes_used(), 0);
    const int64_t empty_size = pool.ram_bytes_used();

    std::vector<uint8_t> payload(static_cast<size_t>(BYTE_BLOCK_SIZE) + 100, 0x2A);
    ASSERT_TRUE(pool.append(std::span<const uint8_t>(payload)).has_value());
    // Two blocks were allocated, so the pool holds at least two blocks of bytes.
    EXPECT_GE(pool.ram_bytes_used(), empty_size + 2 * static_cast<int64_t>(BYTE_BLOCK_SIZE));
}

TEST(AccountableTest, DocIdSetsAccountForTheirContent) {
    // The factory wants a sentinel at `docs[length]`.
    auto int_array = IntArrayDocIdSet::create(std::vector<int32_t>{1, 5, 9, NO_MORE_DOCS}, 3);
    ASSERT_TRUE(int_array.has_value()) << (int_array ? "" : int_array.error().to_string());
    EXPECT_EQ(int_array.value()->ram_bytes_used(), 64 + 4 * static_cast<int64_t>(sizeof(int32_t)));

    auto bits = std::make_shared<FixedBitSet>(128);
    bits->set(3);
    bits->set(100);
    auto bit_set = BitDocIdSet::create(bits);
    ASSERT_TRUE(bit_set.has_value());
    EXPECT_EQ(bit_set.value()->ram_bytes_used(), bits->ram_bytes_used() + 64);

    auto inner = IntArrayDocIdSet::create(std::vector<int32_t>{2, NO_MORE_DOCS}, 1);
    ASSERT_TRUE(inner.has_value());
    NotDocIdSet not_set(200, inner.value());
    EXPECT_EQ(not_set.ram_bytes_used(), 64 + inner.value()->ram_bytes_used());
}

TEST(AccountableTest, WriterStructuresReportTheirFootprint) {
    // A roaring set made of a sparse block.
    RoaringDocIdSet::Builder builder(1 << 16);
    for (int32_t doc : {1, 4, 7}) {
        ASSERT_TRUE(builder.add(doc).has_value());
    }
    auto roaring = builder.build();
    ASSERT_TRUE(roaring.has_value());
    EXPECT_GT(roaring.value()->ram_bytes_used(), 0);

    // The buffered deletes of the writer.
    BufferedUpdates updates;
    const int64_t empty_size = updates.ram_bytes_used();
    ASSERT_TRUE(updates.add_term(Term::from_text("body", "alpha"), 3).has_value());
    EXPECT_GT(updates.ram_bytes_used(), empty_size);
    EXPECT_GT(updates.delete_terms().ram_bytes_used(), 0);

    // A frozen packet reports the size it was frozen with.
    auto packet = FrozenBufferedUpdates::freeze(std::make_unique<BufferedUpdates>(std::move(updates)), std::nullopt);
    ASSERT_TRUE(packet.has_value());
    EXPECT_GE(packet.value()->ram_bytes_used(), 0);
    EXPECT_EQ(packet.value()->ram_bytes_used(), packet.value()->bytes_used());
}

TEST(AccountableTest, HashAndUpdateBuffersReportTheirFootprint) {
    BytesRefHash hash;
    EXPECT_GE(hash.ram_bytes_used(), 0);

    FieldUpdatesBuffer buffer;
    const int64_t empty_size = buffer.ram_bytes_used();
    buffer.add_numeric(Term::from_text("id", "1"), "price", 4, 42);
    EXPECT_GT(buffer.ram_bytes_used(), empty_size);
}
