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
#include <functional>
#include <memory>
#include <span>
#include <vector>

module lucene_core:ut.bkd_tree_reader;

import lucene_core;

using namespace lucene;

namespace {

BKDConfig config_of(size_t dims, size_t index_dims, size_t max_points_in_leaf) {
    auto config = BKDConfig::create(dims, index_dims, 4, max_points_in_leaf);
    EXPECT_TRUE(config.has_value());
    return config.value();
}

std::vector<uint8_t> pack1(int32_t dim0) {
    std::vector<uint8_t> packed(4, 0);
    BitUtil::set_i32_be(packed, 0, dim0);
    return packed;
}

std::vector<uint8_t> pack2(int32_t dim0, int32_t dim1) {
    std::vector<uint8_t> packed(8, 0);
    BitUtil::set_i32_be(packed, 0, dim0);
    BitUtil::set_i32_be(packed, 4, dim1);
    return packed;
}

class MemoryIndexInput final : public IndexInput {
public:
    explicit MemoryIndexInput(std::span<const uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] Result<size_t> get_file_pointer() const override { return pos_; }
    [[nodiscard]] VoidResult seek(size_t pos) override {
        if (pos > bytes_.size()) {
            return err(LuceneError::eof("cannot seek past the end of the buffer"));
        }
        pos_ = pos;
        return {};
    }
    [[nodiscard]] Result<size_t> length() const override { return bytes_.size(); }
    [[nodiscard]] Result<uint8_t> read_byte() override {
        if (pos_ >= bytes_.size()) {
            return err(LuceneError::eof("read past the end of the buffer"));
        }
        return bytes_[pos_++];
    }
    [[nodiscard]] VoidResult read_bytes(std::span<uint8_t> b) override {
        if (pos_ + b.size() > bytes_.size()) {
            return err(LuceneError::eof("read past the end of the buffer"));
        }
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(pos_), b.size(), b.begin());
        pos_ += b.size();
        return {};
    }
    [[nodiscard]] Result<std::unique_ptr<IndexInput>> clone() const override {
        auto copy = std::make_unique<MemoryIndexInput>(bytes_);
        copy->pos_ = pos_;
        return std::unique_ptr<IndexInput>(std::move(copy));
    }

private:
    std::span<const uint8_t> bytes_;
    size_t pos_{0};
};

class MemoryIndexOutput final : public IndexOutput {
public:
    using DataOutput::write_bytes;

    [[nodiscard]] VoidResult write_byte(uint8_t b) override {
        bytes_.push_back(b);
        return {};
    }
    [[nodiscard]] VoidResult write_bytes(std::span<const uint8_t> b, size_t offset, size_t length) override {
        bytes_.insert(bytes_.end(), b.begin() + static_cast<std::ptrdiff_t>(offset),
                      b.begin() + static_cast<std::ptrdiff_t>(offset + length));
        return {};
    }
    [[nodiscard]] Result<size_t> get_file_pointer() const override { return bytes_.size(); }
    [[nodiscard]] uint64_t get_checksum() override { return 0; }
    [[nodiscard]] std::string_view get_name() const override { return "memory"; }
    [[nodiscard]] VoidResult close() override { return {}; }

    [[nodiscard]] const std::vector<uint8_t> &bytes() const noexcept { return bytes_; }

private:
    std::vector<uint8_t> bytes_;
};

/// A written tree plus everything needed to query it.
struct WrittenTree {
    std::vector<uint8_t> data;
    std::vector<uint8_t> meta_bytes;
    BkdMeta meta;
    BkdIndexTree tree;

    [[nodiscard]] bool valid() const noexcept { return !tree.nodes.empty(); }
};

WrittenTree write_tree(const BKDConfig &config, const std::vector<std::vector<uint8_t>> &points, int32_t max_doc) {
    WrittenTree written;
    auto writer_result = BkdWriter::create(config, points.size(), max_doc);
    EXPECT_TRUE(writer_result.has_value());
    if (!writer_result.has_value()) {
        return written;
    }
    auto writer = std::move(writer_result.value());
    for (size_t i = 0; i < points.size(); ++i) {
        EXPECT_TRUE(writer.add(points[i], static_cast<int32_t>(i)).has_value());
    }

    MemoryIndexOutput data;
    auto result = writer.finish(data);
    EXPECT_TRUE(result.has_value());
    if (!result.has_value()) {
        return written;
    }
    written.data = data.bytes();

    MemoryIndexOutput meta_out;
    EXPECT_TRUE(writer.write_index(meta_out, nullptr, *result).has_value());
    written.meta_bytes = meta_out.bytes();

    ByteArrayDataInput meta_in(written.meta_bytes);
    auto meta = BkdMeta::read(meta_in);
    EXPECT_TRUE(meta.has_value());
    if (!meta.has_value()) {
        return written;
    }
    written.meta = std::move(meta.value());

    MemoryIndexInput index_in(written.meta_bytes);
    auto tree = BkdIndexTree::read(index_in, written.meta);
    EXPECT_TRUE(tree.has_value()) << (tree ? "" : tree.error().to_string());
    if (!tree.has_value()) {
        return written;
    }
    written.tree = std::move(tree.value());
    return written;
}

/// Runs a query and returns the matching (doc id, first dimension value) pairs.
std::vector<std::pair<int32_t, int32_t>> query(const WrittenTree &written, std::vector<uint8_t> min,
                                               std::vector<uint8_t> max) {
    MemoryIndexInput data_in(written.data);
    std::vector<std::pair<int32_t, int32_t>> hits;
    auto visited = BkdTreeReader::intersect(written.tree, written.meta, data_in, min, max,
                                            [&hits](int32_t doc_id, std::span<const uint8_t> value) -> VoidResult {
                                                hits.emplace_back(doc_id, BitUtil::get_i32_be(value, 0));
                                                return {};
                                            });
    EXPECT_TRUE(visited.has_value()) << (visited ? "" : visited.error().to_string());
    return hits;
}

} // namespace

TEST(BkdTreeReaderTest, VisitsEveryPointInLeafOrder) {
    const BKDConfig config = config_of(1, 1, 8);
    std::vector<std::vector<uint8_t>> points;
    for (int32_t i = 0; i < 40; ++i) {
        points.push_back(pack1(i));
    }
    const WrittenTree written = write_tree(config, points, 100);
    ASSERT_TRUE(written.valid());

    MemoryIndexInput data_in(written.data);
    std::vector<int32_t> docs;
    std::vector<int32_t> values;
    auto visited = BkdTreeReader::visit_all(written.tree, written.meta, data_in,
                                            [&](int32_t doc_id, std::span<const uint8_t> value) -> VoidResult {
                                                docs.push_back(doc_id);
                                                values.push_back(BitUtil::get_i32_be(value, 0));
                                                return {};
                                            });
    ASSERT_TRUE(visited.has_value()) << (visited ? "" : visited.error().to_string());
    ASSERT_EQ(values.size(), 40u);
    for (size_t i = 0; i < values.size(); ++i) {
        EXPECT_EQ(values[i], static_cast<int32_t>(i));
        EXPECT_EQ(docs[i], static_cast<int32_t>(i));
    }
}

TEST(BkdTreeReaderTest, IntersectsWithAHalfOpenBox) {
    const BKDConfig config = config_of(1, 1, 8);
    std::vector<std::vector<uint8_t>> points;
    for (int32_t i = 0; i < 40; ++i) {
        points.push_back(pack1(i));
    }
    const WrittenTree written = write_tree(config, points, 100);
    ASSERT_TRUE(written.valid());

    {
        const auto hits = query(written, pack1(10), pack1(20));
        ASSERT_EQ(hits.size(), 10u);
        for (size_t i = 0; i < hits.size(); ++i) {
            EXPECT_EQ(hits[i].second, 10 + static_cast<int32_t>(i));
            EXPECT_EQ(hits[i].first, 10 + static_cast<int32_t>(i));
        }
    }
    {
        const auto hits = query(written, pack1(0), pack1(1));
        ASSERT_EQ(hits.size(), 1u);
        EXPECT_EQ(hits[0].second, 0);
    }
    {
        // The upper bound is exclusive, so 39 is only matched by a box ending at 40.
        const auto hits = query(written, pack1(39), pack1(40));
        ASSERT_EQ(hits.size(), 1u);
        EXPECT_EQ(hits[0].second, 39);
    }
    {
        const auto hits = query(written, pack1(40), pack1(100));
        EXPECT_TRUE(hits.empty());
    }
    {
        const auto hits = query(written, pack1(0), pack1(100));
        EXPECT_EQ(hits.size(), 40u);
    }
}

TEST(BkdTreeReaderTest, IntersectsOnEveryIndexedDimension) {
    const BKDConfig config = config_of(2, 2, 6);
    std::vector<std::vector<uint8_t>> points;
    for (int32_t i = 0; i < 20; ++i) {
        points.push_back(pack2(i, 100 - i));
    }
    const WrittenTree written = write_tree(config, points, 40);
    ASSERT_TRUE(written.valid());

    // 5 <= dim0 < 10 and 85 <= dim1 < 96 both have to hold.
    const auto hits = query(written, pack2(5, 85), pack2(10, 96));
    std::vector<int32_t> docs;
    for (const auto &hit : hits) {
        docs.push_back(hit.first);
    }
    EXPECT_EQ(docs, (std::vector<int32_t>{5, 6, 7, 8, 9}));

    // A box that only the data dimension would exclude still matches, because
    // cells are boxes over the indexed dimensions and both dimensions are indexed.
    const auto all = query(written, pack2(0, 81), pack2(20, 101));
    EXPECT_EQ(all.size(), 20u);
}

TEST(BkdTreeReaderTest, PropagatesVisitorFailures) {
    const BKDConfig config = config_of(1, 1, 8);
    std::vector<std::vector<uint8_t>> points;
    for (int32_t i = 0; i < 10; ++i) {
        points.push_back(pack1(i));
    }
    const WrittenTree written = write_tree(config, points, 20);
    ASSERT_TRUE(written.valid());

    MemoryIndexInput data_in(written.data);
    size_t seen = 0;
    auto visited = BkdTreeReader::intersect(written.tree, written.meta, data_in, pack1(0), pack1(20),
                                            [&seen](int32_t, std::span<const uint8_t>) -> VoidResult {
                                                ++seen;
                                                if (seen == 3) {
                                                    return err(LuceneError::time_exceeded("stop"));
                                                }
                                                return {};
                                            });
    ASSERT_FALSE(visited.has_value());
    EXPECT_EQ(visited.error().kind(), ErrorKind::kTimeExceeded);
    EXPECT_EQ(seen, 3u);
}

TEST(BkdTreeReaderTest, RejectsMalformedBoxes) {
    const BKDConfig config = config_of(1, 1, 8);
    std::vector<std::vector<uint8_t>> points{pack1(1), pack1(2)};
    const WrittenTree written = write_tree(config, points, 4);
    ASSERT_TRUE(written.valid());

    MemoryIndexInput data_in(written.data);
    auto visited = BkdTreeReader::intersect(written.tree, written.meta, data_in, std::vector<uint8_t>{0},
                                            std::vector<uint8_t>{1},
                                            [](int32_t, std::span<const uint8_t>) -> VoidResult { return {}; });
    ASSERT_FALSE(visited.has_value());
    EXPECT_EQ(visited.error().kind(), ErrorKind::kIllegalArgument);
}
