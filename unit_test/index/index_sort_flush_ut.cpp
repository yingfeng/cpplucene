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
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.index_sort_flush;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kIdField = "id";
constexpr std::string_view kTagField = "tag";
constexpr std::string_view kNumField = "num";

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqsAndPositions).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

FieldType numeric_type() {
    FieldType type;
    EXPECT_TRUE(type.set_doc_values_type(DocValuesType::Numeric).has_value());
    return type;
}

/// A document with a stored text field (`id`), a sorted doc values field (`tag`)
/// and a numeric doc values field (`num`).
Document make_doc(std::string id, int64_t num) {
    Document doc;
    doc.add(std::make_unique<Field>(std::string(kIdField), FieldValue{id}, text_type()));
    auto tag = SortedDocValuesField::create(std::string(kTagField), id);
    EXPECT_TRUE(tag.has_value()) << (tag ? "" : tag.error().to_string());
    if (tag.has_value()) {
        doc.add(std::make_unique<Field>(std::move(tag.value())));
    }
    auto numeric = Field::from_i64(std::string(kNumField), num, numeric_type());
    EXPECT_TRUE(numeric.has_value()) << (numeric ? "" : numeric.error().to_string());
    if (numeric.has_value()) {
        doc.add(std::make_unique<Field>(std::move(numeric.value())));
    }
    return doc;
}

SortField numeric_sort(bool reverse) {
    auto field = SortField::with_reverse(std::string(kNumField), SortFieldType::kInt, reverse);
    EXPECT_TRUE(field.has_value()) << (field ? "" : field.error().to_string());
    return field.value();
}

SortField tag_sort(bool reverse) {
    auto field = SortField::with_reverse(std::string(kTagField), SortFieldType::kString, reverse);
    EXPECT_TRUE(field.has_value()) << (field ? "" : field.error().to_string());
    return field.value();
}

/// The `id` stored in each document of `leaf`, in doc order.
std::vector<std::string> stored_ids(LeafReader &leaf) {
    std::vector<std::string> ids;
    auto stored = leaf.stored_fields();
    EXPECT_TRUE(stored.has_value()) << (stored ? "" : stored.error().to_string());
    if (!stored.has_value()) {
        return ids;
    }
    auto max = leaf.max_doc();
    EXPECT_TRUE(max.has_value());
    for (int32_t doc = 0; doc < max.value(); ++doc) {
        auto fields = stored.value()->document(doc);
        EXPECT_TRUE(fields.has_value()) << (fields ? "" : fields.error().to_string());
        if (!fields.has_value()) {
            continue;
        }
        auto id = fields.value().get(kIdField);
        if (id.has_value() && id.value().has_value()) {
            ids.push_back(id.value().value());
        }
    }
    return ids;
}

/// The numeric doc values value of every document of `leaf`, in doc order.
std::vector<int64_t> numeric_values(LeafReader &leaf) {
    std::vector<int64_t> values;
    auto cursor = leaf.get_numeric_doc_values(kNumField);
    EXPECT_TRUE(cursor.has_value()) << (cursor ? "" : cursor.error().to_string());
    if (!cursor.has_value() || cursor.value() == nullptr) {
        return values;
    }
    while (true) {
        auto doc = cursor.value()->next_doc();
        EXPECT_TRUE(doc.has_value());
        if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
            break;
        }
        auto value = cursor.value()->long_value();
        EXPECT_TRUE(value.has_value());
        values.push_back(value.value());
    }
    return values;
}

std::shared_ptr<SegmentReader> open_segment(ByteBuffersDirectory &dir, const SegmentCommitInfo &commit) {
    auto reader = SegmentReader::open(dir, commit, LUCENE_CURRENT_VERSION.major, io_context_default());
    EXPECT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    return reader.has_value() ? reader.value() : nullptr;
}

} // namespace

TEST(IndexSortFlushTest, WritesTheDocumentsInSortedOrder) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_merge_policy(NoMergePolicy::create());
    config.set_index_sort(Sort::with_fields({numeric_sort(false)}).value());

    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    for (const auto &[id, num] : std::vector<std::pair<std::string, int64_t>>{{"a", 3}, {"b", 1}, {"c", 2}}) {
        ASSERT_TRUE(writer.value()->add_document(make_doc(id, num)).has_value());
    }
    auto committed = writer.value()->commit();
    ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());

    const std::vector<SegmentCommitInfo> &segments = writer.value()->segment_infos().segments;
    ASSERT_EQ(segments.size(), 1U);
    auto segment = open_segment(dir, segments[0]);
    ASSERT_NE(segment, nullptr);

    EXPECT_EQ(stored_ids(*segment), (std::vector<std::string>{"b", "c", "a"}));
    EXPECT_EQ(numeric_values(*segment), (std::vector<int64_t>{1, 2, 3}));

    // The segment recorded the sort it was written with.
    const std::optional<Sort> &recorded = segments[0].info.sort;
    ASSERT_TRUE(recorded.has_value());
    EXPECT_TRUE(recorded.value().equals(Sort::with_fields({numeric_sort(false)}).value()));
}

TEST(IndexSortFlushTest, ReadsTheSortBackFromTheSegmentMetadata) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_merge_policy(NoMergePolicy::create());
    config.set_index_sort(Sort::with_fields({numeric_sort(true)}).value());

    {
        auto writer = IndexWriter::open(dir, config);
        ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
        ASSERT_TRUE(writer.value()->add_document(make_doc("a", 1)).has_value());
        ASSERT_TRUE(writer.value()->add_document(make_doc("b", 2)).has_value());
        ASSERT_TRUE(writer.value()->commit().has_value());
    }

    // A new writer reads the committed metadata back.
    auto reopened = IndexWriter::open(dir, config);
    ASSERT_TRUE(reopened.has_value()) << (reopened ? "" : reopened.error().to_string());
    const std::vector<SegmentCommitInfo> &segments = reopened.value()->segment_infos().segments;
    ASSERT_EQ(segments.size(), 1U);
    const std::optional<Sort> &recorded = segments[0].info.sort;
    ASSERT_TRUE(recorded.has_value());
    ASSERT_EQ(recorded.value().fields().size(), 1U);
    EXPECT_TRUE(recorded.value().fields()[0].equals(numeric_sort(true)));
    EXPECT_TRUE(recorded.value().fields()[0].reverse());

    auto segment = open_segment(dir, segments[0]);
    ASSERT_NE(segment, nullptr);
    EXPECT_EQ(stored_ids(*segment), (std::vector<std::string>{"b", "a"}));
    EXPECT_EQ(numeric_values(*segment), (std::vector<int64_t>{2, 1}));
}

TEST(IndexSortFlushTest, SortsBySeveralFields) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_merge_policy(NoMergePolicy::create());
    config.set_index_sort(Sort::with_fields({numeric_sort(false), tag_sort(true)}).value());

    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    // `a` and `b` share the numeric value, so the tag decides (descending).
    ASSERT_TRUE(writer.value()->add_document(make_doc("a", 1)).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("b", 1)).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("c", 0)).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    const std::vector<SegmentCommitInfo> &segments = writer.value()->segment_infos().segments;
    ASSERT_EQ(segments.size(), 1U);
    auto segment = open_segment(dir, segments[0]);
    ASSERT_NE(segment, nullptr);
    EXPECT_EQ(stored_ids(*segment), (std::vector<std::string>{"c", "b", "a"}));
}

TEST(IndexSortFlushTest, SortsEveryFlushedSegment) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_merge_policy(NoMergePolicy::create());
    config.set_max_buffered_docs(2);
    config.set_index_sort(Sort::with_fields({numeric_sort(false)}).value());

    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    ASSERT_TRUE(writer.value()->add_document(make_doc("a", 9)).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("b", 7)).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("c", 1)).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("d", 0)).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    const std::vector<SegmentCommitInfo> &segments = writer.value()->segment_infos().segments;
    ASSERT_EQ(segments.size(), 2U);

    auto first = open_segment(dir, segments[0]);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(stored_ids(*first), (std::vector<std::string>{"b", "a"}));
    EXPECT_EQ(numeric_values(*first), (std::vector<int64_t>{7, 9}));

    auto second = open_segment(dir, segments[1]);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(stored_ids(*second), (std::vector<std::string>{"d", "c"}));
    EXPECT_EQ(numeric_values(*second), (std::vector<int64_t>{0, 1}));
}

TEST(IndexSortFlushTest, MergesTwoSortedSegmentsInSortedOrder) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_max_buffered_docs(2);
    config.set_index_sort(Sort::with_fields({numeric_sort(false)}).value());

    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    // Two segments, each sorted on its own: [b(7), a(9)] and [d(0), c(1)].
    ASSERT_TRUE(writer.value()->add_document(make_doc("a", 9)).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("b", 7)).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("c", 1)).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("d", 0)).has_value());
    auto committed = writer.value()->commit();
    ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    ASSERT_EQ(writer.value()->segment_infos().segments.size(), 2U);

    auto merged = writer.value()->force_merge(1);
    ASSERT_TRUE(merged.has_value()) << (merged ? "" : merged.error().to_string());
    const std::vector<SegmentCommitInfo> &segments = writer.value()->segment_infos().segments;
    ASSERT_EQ(segments.size(), 1U);

    // The merge interleaves the segments instead of concatenating them.
    auto segment = open_segment(dir, segments[0]);
    ASSERT_NE(segment, nullptr);
    EXPECT_EQ(stored_ids(*segment), (std::vector<std::string>{"d", "c", "b", "a"}));
    EXPECT_EQ(numeric_values(*segment), (std::vector<int64_t>{0, 1, 7, 9}));

    // The merged segment is still described as sorted.
    const std::optional<Sort> &recorded = segments[0].info.sort;
    ASSERT_TRUE(recorded.has_value());
    EXPECT_TRUE(recorded.value().equals(Sort::with_fields({numeric_sort(false)}).value()));
}

TEST(IndexSortFlushTest, MergesAnUnsortedIndexWithoutClaimingASort) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig unsorted_config(analyzer);
    unsorted_config.set_max_buffered_docs(2);

    {
        // Two segments written without a sort, in insertion order.
        auto writer = IndexWriter::open(dir, unsorted_config);
        ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
        ASSERT_TRUE(writer.value()->add_document(make_doc("a", 9)).has_value());
        ASSERT_TRUE(writer.value()->add_document(make_doc("b", 7)).has_value());
        ASSERT_TRUE(writer.value()->add_document(make_doc("c", 1)).has_value());
        ASSERT_TRUE(writer.value()->add_document(make_doc("d", 0)).has_value());
        ASSERT_TRUE(writer.value()->commit().has_value());
        ASSERT_EQ(writer.value()->segment_infos().segments.size(), 2U);
    }

    // Reopening with a sort must not claim that the old segments are sorted.
    IndexWriterConfig sorted_config(analyzer);
    sorted_config.set_index_sort(Sort::with_fields({numeric_sort(false)}).value());
    auto writer = IndexWriter::open(dir, sorted_config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    auto merged = writer.value()->force_merge(1);
    ASSERT_TRUE(merged.has_value()) << (merged ? "" : merged.error().to_string());
    const std::vector<SegmentCommitInfo> &segments = writer.value()->segment_infos().segments;
    ASSERT_EQ(segments.size(), 1U);
    EXPECT_FALSE(segments[0].info.sort.has_value());

    auto segment = open_segment(dir, segments[0]);
    ASSERT_NE(segment, nullptr);
    EXPECT_EQ(stored_ids(*segment), (std::vector<std::string>{"a", "b", "c", "d"}));
}

TEST(IndexSortFlushTest, DeletesAndSearchesASortedIndex) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_merge_policy(NoMergePolicy::create());
    config.set_index_sort(Sort::with_fields({numeric_sort(false)}).value());

    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    for (const auto &[id, num] : std::vector<std::pair<std::string, int64_t>>{{"a", 3}, {"b", 1}, {"c", 2}}) {
        ASSERT_TRUE(writer.value()->add_document(make_doc(id, num)).has_value());
    }
    ASSERT_TRUE(writer.value()->commit().has_value());

    // The sorted order survives a delete and the reader sees the live documents.
    ASSERT_TRUE(writer.value()->delete_documents(Term(std::string(kIdField), BytesRef::from_string("c"))).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    const std::vector<SegmentCommitInfo> &segments = writer.value()->segment_infos().segments;
    ASSERT_EQ(segments.size(), 1U);
    auto segment = open_segment(dir, segments[0]);
    ASSERT_NE(segment, nullptr);
    EXPECT_EQ(segment->num_docs().value(), 2);
    EXPECT_EQ(segment->num_deleted_docs().value(), 1);
}

TEST(IndexSortFlushTest, RejectsASortThatCannotOrderDocuments) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto score = SortField::score();
    ASSERT_TRUE(score.has_value());
    config.set_index_sort(Sort::with_fields({score.value()}).value());

    auto writer = IndexWriter::open(dir, config);
    ASSERT_FALSE(writer.has_value());
    EXPECT_EQ(writer.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(IndexSortFlushTest, WritesInInsertionOrderWithoutASort) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_merge_policy(NoMergePolicy::create());

    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    ASSERT_TRUE(writer.value()->add_document(make_doc("a", 3)).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("b", 1)).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    const std::vector<SegmentCommitInfo> &segments = writer.value()->segment_infos().segments;
    ASSERT_EQ(segments.size(), 1U);
    auto segment = open_segment(dir, segments[0]);
    ASSERT_NE(segment, nullptr);
    EXPECT_EQ(stored_ids(*segment), (std::vector<std::string>{"a", "b"}));
    EXPECT_FALSE(segments[0].info.sort.has_value());
}
