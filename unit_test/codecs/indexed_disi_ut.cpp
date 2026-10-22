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

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

module lucene_core:ut.indexed_disi;

import lucene_core;

using namespace lucene;

namespace {

/// A [`DocIdSetIterator`] over a sorted vector of doc ids, i.e. what a codec hands
/// to `IndexedDISI::write_bitset`.
class VectorDISI final : public DocIdSetIterator {
public:
    explicit VectorDISI(std::vector<int32_t> docs) : docs_(std::move(docs)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        if (position_ >= docs_.size()) {
            doc_ = NO_MORE_DOCS;
            return doc_;
        }
        doc_ = docs_[position_++];
        return doc_;
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(docs_.size()); }

private:
    std::vector<int32_t> docs_;
    size_t position_{0};
    int32_t doc_{-1};
};

std::vector<int32_t> range_step(int32_t count, int32_t step) {
    std::vector<int32_t> docs;
    docs.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        docs.push_back(i * step);
    }
    return docs;
}

/// Writes `docs` into `name` and reopens the blocks.
struct WrittenDisi {
    DisiWriteResult result;
    std::unique_ptr<IndexedDISI> disi;
    size_t length{0};
};

WrittenDisi write_and_open(Directory &directory, std::string_view name, const std::vector<int32_t> &docs,
                           int8_t dense_rank_power = DEFAULT_DENSE_RANK_POWER) {
    WrittenDisi written;
    VectorDISI source(docs);

    auto out = directory.create_output(name, io_context_default());
    EXPECT_TRUE(out.has_value());
    auto written_result = IndexedDISI::write_bitset(source, *out.value(), dense_rank_power);
    EXPECT_TRUE(written_result.has_value()) << (written_result ? "" : written_result.error().to_string());
    EXPECT_TRUE(out.value()->close().has_value());
    written.result = written_result.value();

    auto length = directory.file_length(name);
    EXPECT_TRUE(length.has_value());
    written.length = length.value();

    auto input = directory.open_input(name, io_context_default());
    EXPECT_TRUE(input.has_value());
    auto disi = IndexedDISI::create(*input.value(), 0, written.length, written.result.jump_table_entry_count,
                                    written.result.dense_rank_power, static_cast<int64_t>(docs.size()));
    EXPECT_TRUE(disi.has_value()) << (disi ? "" : disi.error().to_string());
    written.disi = std::move(disi.value());
    return written;
}

std::vector<int32_t> collect(IndexedDISI &disi) {
    std::vector<int32_t> seen;
    while (true) {
        auto doc = disi.next_doc();
        EXPECT_TRUE(doc.has_value()) << (doc ? "" : doc.error().to_string());
        if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
            break;
        }
        seen.push_back(doc.value());
    }
    return seen;
}

} // namespace

TEST(IndexedDISITest, RoundTripsSparseBlocks) {
    ByteBuffersDirectory directory;
    const std::vector<int32_t> docs{0, 1, 17, 1000, 40000, 65535, 65536, 70000, 131071, 200000};
    auto written = write_and_open(directory, "sparse.disi", docs);

    EXPECT_EQ(collect(*written.disi), docs);

    // `advance` needs a target strictly greater than the current doc id, so every
    // probe gets a fresh iterator.
    auto open = [&]() {
        auto input = directory.open_input("sparse.disi", io_context_default());
        EXPECT_TRUE(input.has_value());
        return IndexedDISI::create(*input.value(), 0, written.length, written.result.jump_table_entry_count,
                                   written.result.dense_rank_power, static_cast<int64_t>(docs.size()));
    };
    for (const int32_t target : {0, 2, 1000, 39999, 65535, 65537, 199999, 200000, 300000}) {
        auto disi = open();
        ASSERT_TRUE(disi.has_value());
        auto advanced = disi.value()->advance(target);
        ASSERT_TRUE(advanced.has_value());
        auto expected = std::lower_bound(docs.begin(), docs.end(), target);
        const int32_t expected_doc = expected == docs.end() ? NO_MORE_DOCS : *expected;
        EXPECT_EQ(advanced.value(), expected_doc) << "target=" << target;
    }

    // `advance_exact` reports presence and pins the doc id either way.
    auto hit_iterator = open();
    ASSERT_TRUE(hit_iterator.has_value());
    auto exact_hit = hit_iterator.value()->advance_exact(200000);
    ASSERT_TRUE(exact_hit.has_value());
    EXPECT_TRUE(exact_hit.value());
    EXPECT_EQ(hit_iterator.value()->doc_id(), 200000);

    auto miss_iterator = open();
    ASSERT_TRUE(miss_iterator.has_value());
    auto exact_miss = miss_iterator.value()->advance_exact(200001);
    ASSERT_TRUE(exact_miss.has_value());
    EXPECT_FALSE(exact_miss.value());
    EXPECT_EQ(miss_iterator.value()->doc_id(), 200001);
}

TEST(IndexedDISITest, RoundTripsDenseBlocksWithRank) {
    ByteBuffersDirectory directory;
    // 5000 doc ids in block 0: above the sparse limit (4095), so the block is a
    // bit set with a rank table.
    const std::vector<int32_t> docs = range_step(5000, 13);
    auto written = write_and_open(directory, "dense.disi", docs);

    EXPECT_EQ(collect(*written.disi), docs);

    auto reopened = directory.open_input("dense.disi", io_context_default());
    ASSERT_TRUE(reopened.has_value());
    auto disi = IndexedDISI::create(*reopened.value(), 0, written.length, written.result.jump_table_entry_count,
                                   written.result.dense_rank_power, static_cast<int64_t>(docs.size()));
    ASSERT_TRUE(disi.has_value());

    // Long advances inside the block exercise the rank skip.
    for (const int32_t target : {0, 129, 130, 4096, 30000, 60000, 64987, 64988, 65535}) {
        auto advanced = disi.value()->advance(target);
        ASSERT_TRUE(advanced.has_value()) << (advanced ? "" : advanced.error().to_string());
        auto expected = std::lower_bound(docs.begin(), docs.end(), target);
        const int32_t expected_doc = expected == docs.end() ? NO_MORE_DOCS : *expected;
        EXPECT_EQ(advanced.value(), expected_doc) << "target=" << target;
        if (expected_doc != NO_MORE_DOCS) {
            EXPECT_EQ(disi.value()->index(), static_cast<int32_t>(expected - docs.begin()));
        }
    }
}

TEST(IndexedDISITest, RoundTripsAnAllBlock) {
    ByteBuffersDirectory directory;
    std::vector<int32_t> docs;
    docs.reserve(DISI_BLOCK_SIZE + 1);
    for (int32_t i = 0; i < DISI_BLOCK_SIZE; ++i) {
        docs.push_back(i);
    }
    // One more doc in the next block, so the reader has to cross the boundary.
    docs.push_back(DISI_BLOCK_SIZE + 5);

    auto written = write_and_open(directory, "all.disi", docs);
    const std::vector<int32_t> seen = collect(*written.disi);
    ASSERT_EQ(seen.size(), docs.size());
    EXPECT_EQ(seen.front(), 0);
    EXPECT_EQ(seen.back(), DISI_BLOCK_SIZE + 5);

    auto reopened = directory.open_input("all.disi", io_context_default());
    ASSERT_TRUE(reopened.has_value());
    auto disi = IndexedDISI::create(*reopened.value(), 0, written.length, written.result.jump_table_entry_count,
                                   written.result.dense_rank_power, static_cast<int64_t>(docs.size()));
    ASSERT_TRUE(disi.has_value());
    auto advanced = disi.value()->advance(40000);
    ASSERT_TRUE(advanced.has_value());
    EXPECT_EQ(advanced.value(), 40000);
    EXPECT_EQ(disi.value()->index(), 40000);
}

TEST(IndexedDISITest, SkipsEmptyBlocksWithTheJumpTable) {
    ByteBuffersDirectory directory;
    // One doc in block 0 and one in block 9: eight empty blocks in between, so the
    // writer keeps a jump table.
    std::vector<int32_t> docs{5, 9 * DISI_BLOCK_SIZE + 7};
    auto written = write_and_open(directory, "jumps.disi", docs);
    EXPECT_GT(written.result.jump_table_entry_count, 0);

    auto reopened = directory.open_input("jumps.disi", io_context_default());
    ASSERT_TRUE(reopened.has_value());
    auto disi = IndexedDISI::create(*reopened.value(), 0, written.length, written.result.jump_table_entry_count,
                                   written.result.dense_rank_power, static_cast<int64_t>(docs.size()));
    ASSERT_TRUE(disi.has_value());

    auto first = disi.value()->next_doc();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first.value(), 5);

    // Jumping straight into the last block uses the table.
    auto second = disi.value()->advance(9 * DISI_BLOCK_SIZE);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second.value(), 9 * DISI_BLOCK_SIZE + 7);
    EXPECT_EQ(disi.value()->index(), 1);

    auto done = disi.value()->next_doc();
    ASSERT_TRUE(done.has_value());
    EXPECT_EQ(done.value(), NO_MORE_DOCS);
}

TEST(IndexedDISITest, OmitsTheJumpTableForASingleBlock) {
    ByteBuffersDirectory directory;
    // Two real blocks: the writer drops the table because a single entry plus the
    // terminating block is not worth the space.
    auto written = write_and_open(directory, "single.disi", std::vector<int32_t>{3, 9});
    EXPECT_EQ(written.result.jump_table_entry_count, 0);
    EXPECT_EQ(written.result.dense_rank_power, DEFAULT_DENSE_RANK_POWER);
    EXPECT_EQ(collect(*written.disi), (std::vector<int32_t>{3, 9}));
}

TEST(IndexedDISITest, RejectsAnInvalidDenseRankPower) {
    ByteBuffersDirectory directory;
    auto out = directory.create_output("bad.disi");
    ASSERT_TRUE(out.has_value());
    std::vector<int32_t> docs{1, 2, 3};
    VectorDISI source(docs);
    EXPECT_FALSE(IndexedDISI::write_bitset(source, *out.value(), /*dense_rank_power=*/3).has_value());
    ASSERT_TRUE(out.value()->close().has_value());

    // An empty block list cannot be read: there is no block header, so the first
    // advance fails instead of silently reporting no documents.
    auto out2 = directory.create_output("empty.disi");
    ASSERT_TRUE(out2.has_value());
    ASSERT_TRUE(out2.value()->close().has_value());
    auto input = directory.open_input("empty.disi");
    ASSERT_TRUE(input.has_value());
    auto disi = IndexedDISI::create(*input.value(), 0, 0, 0, DEFAULT_DENSE_RANK_POWER, 0);
    ASSERT_TRUE(disi.has_value());
    EXPECT_FALSE(disi.value()->next_doc().has_value());
}
