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
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.multi_sorter;

import lucene_core;

using namespace lucene;

namespace {

/// One index with two committed segments, each holding the given sort values.
class Fixture {
public:
    explicit Fixture(const std::vector<std::vector<int64_t>> &segment_values) {
        WhitespaceAnalyzer analyzer;
        auto writer = IndexWriter::open(*directory_, IndexWriterConfig(analyzer));
        EXPECT_TRUE(writer.has_value());
        if (!writer.has_value()) {
            return;
        }
        for (const auto &values : segment_values) {
            for (const int64_t value : values) {
                Document doc;
                auto field = NumericDocValuesField::create("rank", value);
                EXPECT_TRUE(field.has_value()) << (field ? "" : field.error().to_string());
                if (field.has_value()) {
                    doc.add(std::make_unique<Field>(std::move(field.value())));
                }
                auto added = writer.value()->add_document(doc);
                EXPECT_TRUE(added.has_value()) << (added ? "" : added.error().to_string());
            }
            auto committed = writer.value()->commit();
            EXPECT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
        }
        EXPECT_TRUE(writer.value()->close().has_value());

        auto opened = DirectoryReader::open(*directory_);
        EXPECT_TRUE(opened.has_value()) << (opened ? "" : opened.error().to_string());
        if (!opened.has_value()) {
            return;
        }
        reader_ = opened.value();
        auto leaves = reader_->leaves();
        EXPECT_TRUE(leaves.has_value());
        if (!leaves.has_value()) {
            return;
        }
        for (const LeafReaderContext &context : leaves.value()) {
            auto *codec = dynamic_cast<CodecReader *>(context.reader());
            EXPECT_TRUE(codec != nullptr);
            if (codec != nullptr) {
                // The reader owns the leaf, so a non-owning pointer is enough.
                leaves_.push_back(std::shared_ptr<CodecReader>(codec, [](CodecReader *) {}));
            }
        }
    }

    [[nodiscard]] const std::vector<std::shared_ptr<CodecReader>> &leaves() const { return leaves_; }

private:
    std::shared_ptr<ByteBuffersDirectory> directory_ = std::make_shared<ByteBuffersDirectory>();
    std::shared_ptr<DirectoryReader> reader_;
    std::vector<std::shared_ptr<CodecReader>> leaves_;
};

/// A sort on `rank`, ascending unless `reverse`.
[[nodiscard]] Sort rank_sort(bool reverse) {
    auto field = SortField::with_reverse("rank", SortFieldType::kLong, reverse);
    EXPECT_TRUE(field.has_value());
    auto sort = Sort::with_fields({field.value()});
    EXPECT_TRUE(sort.has_value());
    return sort.value();
}

} // namespace

TEST(MultiSorterTest, MergeSortsSegmentsIntoTheSortedOrder) {
    // Each segment is already sorted ascending: {1, 3} and {2, 4}.
    Fixture fixture({{1, 3}, {2, 4}});
    ASSERT_EQ(fixture.leaves().size(), 2U);

    auto maps = MultiSorter::sort(rank_sort(false), fixture.leaves());
    ASSERT_TRUE(maps.has_value()) << (maps ? "" : maps.error().to_string());
    ASSERT_TRUE(maps.value().has_value());
    ASSERT_EQ(maps.value()->size(), 2U);

    // Sorted order: 1 (seg 0 doc 0), 2 (seg 1 doc 0), 3 (seg 0 doc 1), 4 (seg 1 doc 1).
    EXPECT_EQ(maps.value()->at(0)->get(0).value(), 0);
    EXPECT_EQ(maps.value()->at(0)->get(1).value(), 2);
    EXPECT_EQ(maps.value()->at(1)->get(0).value(), 1);
    EXPECT_EQ(maps.value()->at(1)->get(1).value(), 3);
}

TEST(MultiSorterTest, ReversesTheMergedOrder) {
    // Already sorted descending within each segment: {3, 1} and {4, 2}.
    Fixture fixture({{3, 1}, {4, 2}});
    ASSERT_EQ(fixture.leaves().size(), 2U);

    auto maps = MultiSorter::sort(rank_sort(true), fixture.leaves());
    ASSERT_TRUE(maps.has_value()) << (maps ? "" : maps.error().to_string());
    ASSERT_TRUE(maps.value().has_value());

    // Descending: 4 (seg 1 doc 0), 3 (seg 0 doc 0), 2 (seg 1 doc 1), 1 (seg 0 doc 1).
    EXPECT_EQ(maps.value()->at(1)->get(0).value(), 0);
    EXPECT_EQ(maps.value()->at(0)->get(0).value(), 1);
    EXPECT_EQ(maps.value()->at(1)->get(1).value(), 2);
    EXPECT_EQ(maps.value()->at(0)->get(1).value(), 3);
}

TEST(MultiSorterTest, ReportsNoMapWhenSegmentsAreAlreadyInOrder) {
    // Segment 0 holds 1 and 2, segment 1 holds 3 and 4: concatenating the segments
    // is already sorted, so nothing has to be rewritten.
    Fixture fixture({{1, 2}, {3, 4}});
    ASSERT_EQ(fixture.leaves().size(), 2U);

    auto maps = MultiSorter::sort(rank_sort(false), fixture.leaves());
    ASSERT_TRUE(maps.has_value()) << (maps ? "" : maps.error().to_string());
    EXPECT_FALSE(maps.value().has_value());
}

TEST(MultiSorterTest, RejectsTheRelevanceSortAndAnEmptyReaderList) {
    Fixture fixture({{1}, {2}});
    ASSERT_EQ(fixture.leaves().size(), 2U);

    auto score_sort = Sort::relevance();
    ASSERT_TRUE(score_sort.has_value());
    auto by_score = MultiSorter::sort(score_sort.value(), fixture.leaves());
    ASSERT_FALSE(by_score.has_value());
    EXPECT_EQ(by_score.error().kind(), ErrorKind::kIllegalArgument);

    auto empty = MultiSorter::sort(rank_sort(false), {});
    ASSERT_FALSE(empty.has_value());
    EXPECT_EQ(empty.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(SortFieldIndexSorterTest, MapsScalarTypesToSorters) {
    struct Case {
        SortFieldType type;
        std::string_view provider_name;
    };
    const std::vector<Case> cases{{SortFieldType::kInt, "int"},
                                  {SortFieldType::kLong, "long"},
                                  {SortFieldType::kFloat, "float"},
                                  {SortFieldType::kDouble, "double"},
                                  {SortFieldType::kString, "string"}};
    for (const Case &item : cases) {
        auto field = SortField::create("f", item.type);
        ASSERT_TRUE(field.has_value());
        auto sorter = field.value().index_sorter();
        ASSERT_TRUE(sorter.has_value()) << (sorter ? "" : sorter.error().to_string());
        EXPECT_EQ(sorter.value()->provider_name(), item.provider_name);
    }

    // Reversing the field reverses the sorter.
    auto reversed = SortField::with_reverse("f", SortFieldType::kLong, true);
    ASSERT_TRUE(reversed.has_value());
    auto sorter = reversed.value().index_sorter();
    ASSERT_TRUE(sorter.has_value());
    EXPECT_NE(sorter.value()->to_string().find("long"), std::string::npos);

    // Score and document order cannot be used as an index sort, and the
    // multi-valued types are not ported.
    auto score = SortField::score();
    ASSERT_TRUE(score.has_value());
    EXPECT_FALSE(score.value().index_sorter().has_value());

    auto doc = SortField::doc();
    ASSERT_TRUE(doc.has_value());
    EXPECT_FALSE(doc.value().index_sorter().has_value());

    auto multi = SortField::with_selector("f", SortFieldType::kSortedNumeric, SortFieldSelector::kMin);
    ASSERT_TRUE(multi.has_value());
    auto unsupported = multi.value().index_sorter();
    ASSERT_FALSE(unsupported.has_value());
    EXPECT_EQ(unsupported.error().kind(), ErrorKind::kUnsupportedOperation);
}
