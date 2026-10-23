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
#include <optional>
#include <span>
#include <utility>
#include <vector>

module lucene_core:ut.sorting_postings;

import lucene_core;

using namespace lucene;

namespace {

/// The postings of one document in the fake below.
struct FakeDocs {
    int32_t doc{0};
    std::vector<int32_t> positions;
    std::vector<int32_t> starts;
    std::vector<int32_t> ends;
    std::vector<std::vector<uint8_t>> payloads;
};

/// A postings enum over a fixed list of documents.
class FakePostingsEnum final : public PostingsEnum {
public:
    explicit FakePostingsEnum(std::vector<FakeDocs> docs) : docs_(std::move(docs)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        index_ += 1;
        if (index_ >= docs_.size()) {
            doc_ = NO_MORE_DOCS;
            return doc_;
        }
        doc_ = docs_[index_].doc;
        position_index_ = 0;
        return doc_;
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        while (true) {
            auto doc = next_doc();
            if (!doc.has_value() || doc.value() == NO_MORE_DOCS || doc.value() >= target) {
                return doc;
            }
        }
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(docs_.size()); }

    [[nodiscard]] Result<int32_t> freq() override { return static_cast<int32_t>(docs_[index_].positions.size()); }

    [[nodiscard]] Result<int32_t> next_position() override {
        return docs_[index_].positions[position_index_++];
    }

    [[nodiscard]] Result<int32_t> start_offset() const override { return docs_[index_].starts[position_index_ - 1]; }
    [[nodiscard]] Result<int32_t> end_offset() const override { return docs_[index_].ends[position_index_ - 1]; }

    [[nodiscard]] Result<std::optional<std::span<const uint8_t>>> get_payload() const override {
        const std::vector<uint8_t> &payload = docs_[index_].payloads[position_index_ - 1];
        if (payload.empty()) {
            return std::optional<std::span<const uint8_t>>{};
        }
        return std::optional<std::span<const uint8_t>>{std::span<const uint8_t>(payload)};
    }

private:
    std::vector<FakeDocs> docs_;
    size_t index_{static_cast<size_t>(-1)};
    size_t position_index_{0};
    int32_t doc_{-1};
};

/// Three documents, the second one with two positions.
[[nodiscard]] std::vector<FakeDocs> sample_docs() {
    std::vector<FakeDocs> docs;
    docs.push_back(FakeDocs{0, {1}, {10}, {14}, {{0xAA}}});
    docs.push_back(FakeDocs{1, {2, 5}, {20, 40}, {24, 44}, {{0xBB}, {}}});
    docs.push_back(FakeDocs{2, {7}, {70}, {74}, {{0xCC}}});
    return docs;
}

} // namespace

TEST(SortingPostingsTest, ReplaysPostingsInTheSortedOrder) {
    auto fake = std::make_unique<FakePostingsEnum>(sample_docs());
    IndexSortDocMapImpl doc_map(std::vector<int32_t>{2, 1, 0});
    auto postings = SortingPostingsEnum::create(std::move(fake), doc_map, ALL);
    ASSERT_TRUE(postings.has_value()) << (postings ? "" : postings.error().to_string());

    // Sorted document 0 is old document 2, with one position at 7.
    EXPECT_EQ(postings.value()->next_doc().value(), 0);
    EXPECT_EQ(postings.value()->freq().value(), 1);
    EXPECT_EQ(postings.value()->next_position().value(), 7);
    EXPECT_EQ(postings.value()->start_offset().value(), 70);
    EXPECT_EQ(postings.value()->end_offset().value(), 74);
    auto payload = postings.value()->get_payload();
    ASSERT_TRUE(payload.has_value());
    ASSERT_TRUE(payload.value().has_value());
    EXPECT_EQ(payload.value().value().size(), 1U);
    EXPECT_EQ(payload.value().value()[0], 0xCC);

    // Sorted document 1 is old document 1, with two positions.
    EXPECT_EQ(postings.value()->next_doc().value(), 1);
    EXPECT_EQ(postings.value()->freq().value(), 2);
    EXPECT_EQ(postings.value()->next_position().value(), 2);
    EXPECT_EQ(postings.value()->start_offset().value(), 20);
    EXPECT_EQ(postings.value()->next_position().value(), 5);
    EXPECT_EQ(postings.value()->start_offset().value(), 40);
    auto empty_payload = postings.value()->get_payload();
    ASSERT_TRUE(empty_payload.has_value());
    EXPECT_FALSE(empty_payload.value().has_value());

    EXPECT_EQ(postings.value()->next_doc().value(), 2);
    EXPECT_EQ(postings.value()->next_position().value(), 1);
    EXPECT_EQ(postings.value()->next_doc().value(), NO_MORE_DOCS);
    EXPECT_EQ(postings.value()->cost().value(), 3);
}

TEST(SortingPostingsTest, AdvancesToTheNextNewDocument) {
    auto fake = std::make_unique<FakePostingsEnum>(sample_docs());
    IndexSortDocMapImpl doc_map(std::vector<int32_t>{2, 1, 0});
    auto postings = SortingPostingsEnum::create(std::move(fake), doc_map, FREQS);
    ASSERT_TRUE(postings.has_value());

    EXPECT_EQ(postings.value()->advance(1).value(), 1);
    EXPECT_EQ(postings.value()->freq().value(), 2);
    EXPECT_EQ(postings.value()->advance(3).value(), NO_MORE_DOCS);
}

TEST(SortingPostingsTest, ReportsNoOffsetsWhenTheyWereNotRequested) {
    auto fake = std::make_unique<FakePostingsEnum>(sample_docs());
    IndexSortDocMapImpl doc_map(std::vector<int32_t>{2, 1, 0});
    auto postings = SortingPostingsEnum::create(std::move(fake), doc_map, POSITIONS);
    ASSERT_TRUE(postings.has_value());

    ASSERT_EQ(postings.value()->next_doc().value(), 0);
    ASSERT_EQ(postings.value()->next_position().value(), 7);
    // Without the offsets flag the wrapper does not pretend to have them.
    EXPECT_EQ(postings.value()->start_offset().value(), -1);
    EXPECT_EQ(postings.value()->end_offset().value(), -1);
}

TEST(SortingPostingsTest, DropsDeletedDocuments) {
    auto fake = std::make_unique<FakePostingsEnum>(sample_docs());
    // Old doc 1 was deleted, so the sorted view only has two documents.
    IndexSortDocMapImpl doc_map(std::vector<int32_t>{2, 0}, 3);
    auto postings = SortingPostingsEnum::create(std::move(fake), doc_map, ALL);
    ASSERT_TRUE(postings.has_value());

    EXPECT_EQ(postings.value()->next_doc().value(), 0);
    EXPECT_EQ(postings.value()->freq().value(), 1);
    EXPECT_EQ(postings.value()->next_doc().value(), 1);
    EXPECT_EQ(postings.value()->next_position().value(), 1);
    EXPECT_EQ(postings.value()->next_doc().value(), NO_MORE_DOCS);
    EXPECT_EQ(postings.value()->cost().value(), 2);
}

TEST(SortingPostingsTest, RejectsMissingPostings) {
    IndexSortDocMapImpl doc_map(std::vector<int32_t>{0});
    auto postings = SortingPostingsEnum::create(nullptr, doc_map, ALL);
    ASSERT_FALSE(postings.has_value());
    EXPECT_EQ(postings.error().kind(), ErrorKind::kIllegalArgument);
}
