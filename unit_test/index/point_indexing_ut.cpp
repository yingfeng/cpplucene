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
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.point_indexing;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kPointField = "num";
constexpr std::string_view kTitleField = "title";

/// A one-dimension, 4-byte point field holding the sortable encoding of `value`.
std::unique_ptr<IndexableField> point_field(std::string_view name, int32_t value) {
    FieldType type;
    EXPECT_TRUE(type.set_dimensions(1, 4).has_value());
    type.freeze();
    std::vector<uint8_t> packed(4);
    NumericUtils::int_to_sortable_bytes(value, packed, 0);
    return std::make_unique<Field>(std::string(name), FieldValue{std::move(packed)}, std::move(type));
}

/// A text field, so the segment is not made of points only.
std::unique_ptr<IndexableField> title_field(std::string value) {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqsAndPositions).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    type.freeze();
    return std::make_unique<Field>(std::string(kTitleField), FieldValue{std::move(value)}, std::move(type));
}

std::vector<uint8_t> packed_int(int32_t value) {
    std::vector<uint8_t> packed(4);
    NumericUtils::int_to_sortable_bytes(value, packed, 0);
    return packed;
}

std::shared_ptr<DirectoryReader> open_reader(ByteBuffersDirectory &dir) {
    auto reader = DirectoryReader::open(dir);
    EXPECT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    return reader.has_value() ? reader.value() : nullptr;
}

std::vector<int32_t> search_docs(std::shared_ptr<DirectoryReader> &reader, const Query &query) {
    std::vector<int32_t> docs;
    IndexSearcher searcher(reader);
    auto top = searcher.search(query, 10);
    EXPECT_TRUE(top.has_value()) << (top ? "" : top.error().to_string());
    if (!top.has_value()) {
        return docs;
    }
    for (const ScoreDoc &hit : top.value().score_docs) {
        docs.push_back(hit.doc);
    }
    return docs;
}

std::vector<int32_t> point_hits(std::shared_ptr<DirectoryReader> &reader, int32_t lower, int32_t upper) {
    auto query = PointRangeQuery::create(std::string(kPointField), packed_int(lower), packed_int(upper), 1);
    EXPECT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());
    if (!query.has_value()) {
        return {};
    }
    return search_docs(reader, query.value());
}

} // namespace

TEST(PointIndexingTest, IndexesNumericPointsAndSearchesThem) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    const std::vector<int32_t> values{5, 1, 9, 3};
    for (size_t index = 0; index < values.size(); ++index) {
        Document doc;
        doc.add(point_field(kPointField, values[index]));
        doc.add(title_field("doc" + std::to_string(index)));
        ASSERT_TRUE(writer.value()->add_document(doc).has_value());
    }
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto reader = open_reader(dir);
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(reader->num_docs().value(), 4);

    // Documents 0 (5), 3 (3) and 1 (1) are inside [1, 5]; document 2 (9) is not.
    auto hits = point_hits(reader, 1, 5);
    EXPECT_EQ(hits.size(), 3U);
    EXPECT_EQ(std::ranges::find(hits, 2), hits.end());

    EXPECT_EQ(point_hits(reader, 9, 9), (std::vector<int32_t>{2}));
    EXPECT_TRUE(point_hits(reader, 100, 200).empty());
}

TEST(PointIndexingTest, WritesThePointFiles) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value());

    Document doc;
    doc.add(point_field(kPointField, 42));
    ASSERT_TRUE(writer.value()->add_document(doc).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto names = dir.list_all();
    ASSERT_TRUE(names.has_value());
    const auto has_extension = [&names](std::string_view extension) {
        return std::ranges::any_of(names.value(), [extension](const std::string &name) {
            return name.size() > extension.size() &&
                   name.compare(name.size() - extension.size(), extension.size(), extension) == 0;
        });
    };
    EXPECT_TRUE(has_extension(".kdd"));
    EXPECT_TRUE(has_extension(".kdi"));
    EXPECT_TRUE(has_extension(".kdm"));
}

TEST(PointIndexingTest, IndexesMultiDimensionalPoints) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value());

    // Two dimensions of four bytes each: (10, 20) and (30, 40).
    const auto add_point = [&writer](int32_t first, int32_t second) {
        FieldType type;
        EXPECT_TRUE(type.set_dimensions(2, 4).has_value());
        type.freeze();
        std::vector<uint8_t> packed(8);
        NumericUtils::int_to_sortable_bytes(first, packed, 0);
        NumericUtils::int_to_sortable_bytes(second, packed, 4);
        Document doc;
        doc.add(std::make_unique<Field>(std::string(kPointField), FieldValue{std::move(packed)}, type));
        EXPECT_TRUE(writer.value()->add_document(doc).has_value());
    };
    add_point(10, 20);
    add_point(30, 40);
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto reader = open_reader(dir);
    ASSERT_NE(reader, nullptr);

    std::vector<uint8_t> lower(8);
    std::vector<uint8_t> upper(8);
    NumericUtils::int_to_sortable_bytes(5, lower, 0);
    NumericUtils::int_to_sortable_bytes(15, upper, 0);
    NumericUtils::int_to_sortable_bytes(15, lower, 4);
    NumericUtils::int_to_sortable_bytes(25, upper, 4);
    auto query = PointRangeQuery::create(std::string(kPointField), lower, upper, 2);
    ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());
    EXPECT_EQ(search_docs(reader, query.value()), (std::vector<int32_t>{0}));
}

TEST(PointIndexingTest, RejectsValuesWithTheWrongLength) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value());

    FieldType type;
    ASSERT_TRUE(type.set_dimensions(1, 4).has_value());
    type.freeze();
    Document doc;
    doc.add(std::make_unique<Field>(std::string(kPointField), FieldValue{std::vector<uint8_t>(8, 0)}, type));
    auto added = writer.value()->add_document(doc);
    ASSERT_FALSE(added.has_value());
    EXPECT_EQ(added.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(PointIndexingTest, RejectsAChangeOfDimensions) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value());

    Document first;
    first.add(point_field(kPointField, 1));
    ASSERT_TRUE(writer.value()->add_document(first).has_value());

    FieldType type;
    ASSERT_TRUE(type.set_dimensions(2, 4).has_value());
    type.freeze();
    Document second;
    second.add(std::make_unique<Field>(std::string(kPointField), FieldValue{std::vector<uint8_t>(8, 0)}, type));
    auto added = writer.value()->add_document(second);
    ASSERT_FALSE(added.has_value());
    EXPECT_EQ(added.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(PointIndexingTest, MergesSegmentsWithPoints) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value());

    for (const int32_t value : {2, 4, 6}) {
        Document doc;
        doc.add(point_field(kPointField, value));
        ASSERT_TRUE(writer.value()->add_document(doc).has_value());
        ASSERT_TRUE(writer.value()->commit().has_value());
    }
    ASSERT_EQ(writer.value()->segment_infos().segments.size(), 3U);

    ASSERT_TRUE(writer.value()->force_merge(1).has_value());
    ASSERT_EQ(writer.value()->segment_infos().segments.size(), 1U);
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto reader = open_reader(dir);
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(reader->num_docs().value(), 3);
    EXPECT_EQ(point_hits(reader, 0, 10).size(), 3U);
    EXPECT_EQ(point_hits(reader, 3, 5), (std::vector<int32_t>{1}));
}

TEST(PointIndexingTest, SurvivesAReopen) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    {
        auto writer = IndexWriter::open(dir, config);
        ASSERT_TRUE(writer.has_value());
        Document doc;
        doc.add(point_field(kPointField, 7));
        ASSERT_TRUE(writer.value()->add_document(doc).has_value());
        ASSERT_TRUE(writer.value()->commit().has_value());
    }

    auto reader = open_reader(dir);
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(point_hits(reader, 0, 10), (std::vector<int32_t>{0}));
}
