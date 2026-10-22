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
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.segment_merger;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kIdField = "id";
constexpr std::string_view kTitleField = "title";
constexpr std::string_view kTagField = "tag";
constexpr std::string_view kNumField = "num";

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqsAndPositions).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    EXPECT_TRUE(type.set_store_term_vectors(true).has_value());
    return type;
}

FieldType numeric_doc_values_type() {
    FieldType type;
    EXPECT_TRUE(type.set_doc_values_type(DocValuesType::Numeric).has_value());
    return type;
}

void add_text(Document &doc, std::string_view name, std::string value) {
    doc.add(std::make_unique<Field>(std::string(name), FieldValue{std::move(value)}, text_type()));
}

/// A document with a stored/indexed text field, a sorted-set doc values field
/// and a numeric doc values field.
Document make_doc(std::string id, std::string title, std::string tag, int64_t num) {
    Document doc;
    add_text(doc, kIdField, std::move(id));
    add_text(doc, kTitleField, std::move(title));
    auto keyword = KeywordField::create(std::string(kTagField), std::move(tag), false);
    EXPECT_TRUE(keyword.has_value()) << (keyword ? "" : keyword.error().to_string());
    if (keyword.has_value()) {
        doc.add(std::make_unique<Field>(std::move(keyword.value())));
    }
    auto numeric = Field::from_i64(std::string(kNumField), num, numeric_doc_values_type());
    EXPECT_TRUE(numeric.has_value()) << (numeric ? "" : numeric.error().to_string());
    if (numeric.has_value()) {
        doc.add(std::make_unique<Field>(std::move(numeric.value())));
    }
    return doc;
}

Term id_term(std::string_view value) {
    return Term(std::string(kIdField), BytesRef::from_string(value));
}

std::shared_ptr<SegmentReader> open_segment(ByteBuffersDirectory &dir, const SegmentCommitInfo &commit) {
    auto reader = SegmentReader::open(dir, commit, LUCENE_CURRENT_VERSION.major, io_context_default());
    EXPECT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    return reader.has_value() ? reader.value() : nullptr;
}

/// The `id` stored in each document of `reader`, in doc order.
std::vector<std::string> stored_ids(LeafReader &leaf) {
    std::vector<std::string> ids;
    auto stored = leaf.stored_fields();
    EXPECT_TRUE(stored.has_value()) << (stored ? "" : stored.error().to_string());
    if (!stored.has_value()) {
        return ids;
    }
    for (int32_t doc = 0; doc < leaf.max_doc().value(); ++doc) {
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

/// The numeric doc values value of every document of the merged segment.
std::vector<int64_t> numeric_values(LeafReader &leaf) {
    std::vector<int64_t> values;
    auto cursor = leaf.get_numeric_doc_values(kNumField);
    EXPECT_TRUE(cursor.has_value()) << (cursor ? "" : cursor.error().to_string());
    if (!cursor.has_value()) {
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

/// The sorted-set values of every document of the merged segment, as ordinals.
std::vector<std::vector<int64_t>> sorted_set_ords(LeafReader &leaf) {
    std::vector<std::vector<int64_t>> result;
    auto cursor = leaf.get_sorted_set_doc_values(kTagField);
    EXPECT_TRUE(cursor.has_value()) << (cursor ? "" : cursor.error().to_string());
    if (!cursor.has_value()) {
        return result;
    }
    while (true) {
        auto doc = cursor.value()->next_doc();
        EXPECT_TRUE(doc.has_value());
        if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
            break;
        }
        std::vector<int64_t> ords;
        auto count = cursor.value()->doc_value_count();
        EXPECT_TRUE(count.has_value());
        for (int32_t i = 0; i < count.value(); ++i) {
            auto ord = cursor.value()->next_ord();
            EXPECT_TRUE(ord.has_value());
            ords.push_back(ord.value());
        }
        result.push_back(std::move(ords));
    }
    return result;
}

int64_t doc_freq(LeafReader &leaf, std::string_view field, std::string_view text) {
    auto terms = leaf.terms(std::string(field));
    EXPECT_TRUE(terms.has_value()) << (terms ? "" : terms.error().to_string());
    if (!terms.has_value() || terms.value() == nullptr) {
        return -1;
    }
    auto iterator = terms.value()->iterator();
    EXPECT_TRUE(iterator.has_value());
    auto seeked = iterator.value()->seek_exact(BytesRef::from_string(text).as_slice());
    EXPECT_TRUE(seeked.has_value());
    if (!seeked.has_value() || !seeked.value()) {
        return 0;
    }
    auto freq = iterator.value()->doc_freq();
    EXPECT_TRUE(freq.has_value());
    return freq.has_value() ? freq.value() : -1;
}

} // namespace

TEST(SegmentMergerTest, MergesTwoSegmentsAndDropsDeletedDocuments) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    // The test drives the merge itself, so the writer must not merge first.
    config.set_merge_policy(NoMergePolicy::create());
    config.set_max_buffered_docs(2);

    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    ASSERT_TRUE(writer.value()->add_document(make_doc("a", "hello world", "x", 10)).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("b", "hello lucene", "y", 20)).has_value());
    // Adding the third document flushes the first two into segment `_0`.
    ASSERT_TRUE(writer.value()->add_document(make_doc("c", "world", "x", 30)).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_EQ(writer.value()->segment_infos().segments.size(), 2U);
    ASSERT_EQ(writer.value()->segment_infos().segments[0].info.name, "_0");

    // Deleting `b` has to survive the merge: it becomes a deleted doc of `_0`.
    ASSERT_TRUE(writer.value()->delete_documents(id_term("b")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    const auto &segments = writer.value()->segment_infos().segments;
    std::vector<std::shared_ptr<SegmentReader>> readers;
    std::vector<CodecReader *> raw_readers;
    for (const SegmentCommitInfo &commit : segments) {
        auto reader = open_segment(dir, commit);
        ASSERT_NE(reader, nullptr);
        raw_readers.push_back(reader.get());
        readers.push_back(std::move(reader));
    }

    SegmentInfo merged_info;
    merged_info.name = "_99";
    merged_info.id.assign(ID_LENGTH, 0x42);
    merged_info.codec_name = std::string(CodecFormats::default_codec().name());
    merged_info.version = LUCENE_CURRENT_VERSION;

    auto merge_context = IOContext::with_merge(MergeInfo(3, 0, false, 2));
    ASSERT_TRUE(merge_context.has_value()) << (merge_context ? "" : merge_context.error().to_string());

    auto merger = SegmentMerger::create(raw_readers, merged_info, std::make_shared<InfoStreamEnum>(), dir,
                                        merge_context.value());
    ASSERT_TRUE(merger.has_value()) << (merger ? "" : merger.error().to_string());
    EXPECT_TRUE(merger.value()->should_merge());
    // Three documents in, one deleted: the merged segment has two.
    EXPECT_EQ(merged_info.max_doc, 2);
    ASSERT_EQ(merger.value()->id().size(), ID_LENGTH);

    auto merged = merger.value()->merge();
    ASSERT_TRUE(merged.has_value()) << (merged ? "" : merged.error().to_string());

    SegmentCommitInfo merged_commit;
    merged_commit.info = merged_info;
    auto reader = open_segment(dir, merged_commit);
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(reader->max_doc().value(), 2);
    EXPECT_EQ(reader->num_docs().value(), 2);

    // The deleted document is gone and the surviving ones are in merge order.
    EXPECT_EQ(stored_ids(*reader), (std::vector<std::string>{"a", "c"}));

    // Postings are merged, with the deleted document's terms dropped.
    EXPECT_EQ(doc_freq(*reader, kTitleField, "hello"), 1);
    EXPECT_EQ(doc_freq(*reader, kTitleField, "world"), 2);
    EXPECT_EQ(doc_freq(*reader, kTitleField, "lucene"), 0);

    // Doc values follow the same document order.
    EXPECT_EQ(numeric_values(*reader), (std::vector<int64_t>{10, 30}));
    const auto tag_ords = sorted_set_ords(*reader);
    ASSERT_EQ(tag_ords.size(), 2U);
    EXPECT_EQ(tag_ords[0], (std::vector<int64_t>{0}));
    EXPECT_EQ(tag_ords[1], (std::vector<int64_t>{0}));
}

TEST(SegmentMergerTest, MergesASingleSegmentAndRenumbersDocValues) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);

    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value());
    auto added = writer.value()->add_document(make_doc("a", "alpha", "x", 1));
    ASSERT_TRUE(added.has_value()) << (added ? "" : added.error().to_string());
    ASSERT_TRUE(writer.value()->add_document(make_doc("b", "beta", "y", 2)).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("c", "gamma", "x", 3)).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_EQ(writer.value()->segment_infos().segments.size(), 1U);

    auto first = open_segment(dir, writer.value()->segment_infos().segments[0]);
    ASSERT_NE(first, nullptr);
    std::vector<CodecReader *> raw_readers{first.get()};

    SegmentInfo merged_info;
    merged_info.name = "_98";
    merged_info.id.assign(ID_LENGTH, 0x43);
    merged_info.codec_name = std::string(CodecFormats::default_codec().name());
    merged_info.version = LUCENE_CURRENT_VERSION;

    auto merge_context = IOContext::with_merge(MergeInfo(3, 0, false, 1));
    ASSERT_TRUE(merge_context.has_value());
    auto merger = SegmentMerger::create(raw_readers, merged_info, std::make_shared<InfoStreamEnum>(), dir,
                                        merge_context.value());
    ASSERT_TRUE(merger.has_value()) << (merger ? "" : merger.error().to_string());
    ASSERT_EQ(merged_info.max_doc, 3);
    ASSERT_TRUE(merger.value()->merge().has_value());

    SegmentCommitInfo merged_commit;
    merged_commit.info = merged_info;
    auto reader = open_segment(dir, merged_commit);
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(reader->num_docs().value(), 3);
    EXPECT_EQ(stored_ids(*reader), (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_EQ(numeric_values(*reader), (std::vector<int64_t>{1, 2, 3}));
    EXPECT_EQ(doc_freq(*reader, kTitleField, "alpha"), 1);

    // The sorted set dictionary is rebuilt, so "x" is ord 0 and "y" is ord 1.
    const auto tag_ords = sorted_set_ords(*reader);
    ASSERT_EQ(tag_ords.size(), 3U);
    EXPECT_EQ(tag_ords[0], (std::vector<int64_t>{0}));
    EXPECT_EQ(tag_ords[1], (std::vector<int64_t>{1}));
    EXPECT_EQ(tag_ords[2], (std::vector<int64_t>{0}));

    // The keyword field is indexed as well, so its binary terms are merged too.
    EXPECT_EQ(doc_freq(*reader, kTagField, "x"), 2);
    EXPECT_EQ(doc_freq(*reader, kTagField, "y"), 1);
}

TEST(SegmentMergerTest, RejectsANonMergeIOContext) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);

    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("a", "alpha", "x", 1)).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto first = open_segment(dir, writer.value()->segment_infos().segments[0]);
    ASSERT_NE(first, nullptr);
    std::vector<CodecReader *> raw_readers{first.get()};

    SegmentInfo merged_info;
    merged_info.name = "_97";
    merged_info.id.assign(ID_LENGTH, 0x44);
    merged_info.codec_name = std::string(CodecFormats::default_codec().name());

    auto rejected = SegmentMerger::create(raw_readers, merged_info, std::make_shared<InfoStreamEnum>(), dir,
                                          io_context_default());
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(SegmentMergerTest, RefusesToMergeIntoAnEmptySegment) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    // The test drives the merge itself, so the writer must not merge first.
    config.set_merge_policy(NoMergePolicy::create());

    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("a", "alpha", "x", 1)).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->delete_all().has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto first = open_segment(dir, writer.value()->segment_infos().segments[0]);
    ASSERT_NE(first, nullptr);
    std::vector<CodecReader *> raw_readers{first.get()};

    SegmentInfo merged_info;
    merged_info.name = "_96";
    merged_info.id.assign(ID_LENGTH, 0x45);
    merged_info.codec_name = std::string(CodecFormats::default_codec().name());

    auto merge_context = IOContext::with_merge(MergeInfo(1, 0, false, 1));
    ASSERT_TRUE(merge_context.has_value());
    auto merger = SegmentMerger::create(raw_readers, merged_info, std::make_shared<InfoStreamEnum>(), dir,
                                        merge_context.value());
    ASSERT_TRUE(merger.has_value()) << (merger ? "" : merger.error().to_string());
    EXPECT_FALSE(merger.value()->should_merge());
    auto merged = merger.value()->merge();
    ASSERT_FALSE(merged.has_value());
    EXPECT_EQ(merged.error().kind(), ErrorKind::kIllegalState);
}
