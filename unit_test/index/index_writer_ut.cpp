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

module lucene_core:ut.index_writer;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kIdField = "id";
constexpr std::string_view kTitleField = "title";

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqsAndPositions).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

void add_text(Document &doc, std::string_view name, std::string value) {
    doc.add(std::make_unique<Field>(std::string(name), FieldValue{std::move(value)}, text_type()));
}

Document make_doc(std::string id, std::string title) {
    Document doc;
    add_text(doc, kIdField, std::move(id));
    add_text(doc, kTitleField, std::move(title));
    return doc;
}

Term id_term(std::string_view value) {
    return Term(std::string(kIdField), BytesRef::from_string(value));
}

std::shared_ptr<DirectoryReader> open_reader(ByteBuffersDirectory &dir) {
    auto reader = DirectoryReader::open(dir);
    EXPECT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    return reader.has_value() ? reader.value() : nullptr;
}

std::vector<int32_t> hits_of(std::shared_ptr<DirectoryReader> &reader, std::string_view field,
                             std::string_view text) {
    std::vector<int32_t> docs;
    IndexSearcher searcher(reader);
    TermQuery query(Term(std::string(field), BytesRef::from_string(text)));
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

} // namespace

TEST(IndexWriterTest, AddsDocumentsAndCommits) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);

    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    ASSERT_TRUE(writer.value()->add_document(make_doc("a", "hello world")).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("b", "hello lucene")).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("c", "world")).has_value());
    EXPECT_EQ(writer.value()->num_docs().value(), 3);
    // Nothing is written before the commit.
    EXPECT_TRUE(SegmentInfos::list_generations(dir).value().empty());

    ASSERT_TRUE(writer.value()->commit().has_value());
    EXPECT_EQ(SegmentInfos::list_generations(dir).value().size(), 1U);
    EXPECT_EQ(writer.value()->segment_infos().segments.size(), 1U);

    auto reader = open_reader(dir);
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(reader->num_docs().value(), 3);
    EXPECT_EQ(reader->max_doc().value(), 3);
    EXPECT_EQ(hits_of(reader, kTitleField, "hello").size(), 2U);
    EXPECT_EQ(hits_of(reader, kTitleField, "world").size(), 2U);
    EXPECT_EQ(hits_of(reader, kTitleField, "missing").size(), 0U);

    // A stored field comes back through the reader.
    LeafReader &leaf = *reader->leaves().value()[0].reader();
    auto stored = leaf.stored_fields();
    ASSERT_TRUE(stored.has_value());
    auto doc = stored.value()->document(1);
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc.value().get(kIdField).value().value(), "b");
}

TEST(IndexWriterTest, AppendsToAnExistingIndex) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);

    {
        auto writer = IndexWriter::open(dir, config);
        ASSERT_TRUE(writer.has_value());
        ASSERT_TRUE(writer.value()->add_document(make_doc("a", "first")).has_value());
        ASSERT_TRUE(writer.value()->commit().has_value());
    }
    {
        // Reopening keeps the committed documents and continues the segment
        // counter, so the new segment is `_1`.
        auto writer = IndexWriter::open(dir, config);
        ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
        EXPECT_EQ(writer.value()->num_docs().value(), 1);
        ASSERT_TRUE(writer.value()->add_document(make_doc("b", "second")).has_value());
        ASSERT_TRUE(writer.value()->commit().has_value());
        EXPECT_EQ(writer.value()->segment_infos().segments.size(), 2U);
        EXPECT_EQ(writer.value()->segment_infos().segments[1].info.name, "_1");
    }

    auto reader = open_reader(dir);
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(reader->num_docs().value(), 2);
    EXPECT_EQ(hits_of(reader, kTitleField, "first").size(), 1U);
    EXPECT_EQ(hits_of(reader, kTitleField, "second").size(), 1U);
}

TEST(IndexWriterTest, FlushesWhenTheBufferIsFull) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_max_buffered_docs(2);

    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("a", "one")).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("b", "two")).has_value());
    EXPECT_TRUE(writer.value()->segment_infos().segments.empty());

    // Adding the third document flushes the first two into a segment, which waits
    // in the flush queue until the commit publishes it.
    ASSERT_TRUE(writer.value()->add_document(make_doc("c", "three")).has_value());
    EXPECT_EQ(writer.value()->flush_count(), 1);
    EXPECT_EQ(writer.value()->pending_flush_count(), 1U);
    EXPECT_TRUE(writer.value()->segment_infos().segments.empty()) << "a flushed segment is not committed yet";
    EXPECT_EQ(writer.value()->num_docs().value(), 3) << "the queued documents count as pending";

    ASSERT_TRUE(writer.value()->commit().has_value());
    EXPECT_EQ(writer.value()->segment_infos().segments.size(), 2U);

    auto reader = open_reader(dir);
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(reader->num_docs().value(), 3);
    EXPECT_EQ(reader->leaves().value().size(), 2U);
    EXPECT_EQ(hits_of(reader, kTitleField, "two").size(), 1U);
}

TEST(IndexWriterTest, DeletesByTermAtCommit) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    // These assertions are about delete bookkeeping, which a merge would
    // reclaim, so merging is turned off.
    config.set_merge_policy(NoMergePolicy::create());

    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("a", "one")).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("b", "two")).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("c", "three")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    ASSERT_TRUE(writer.value()->delete_documents(id_term("b")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto reader = open_reader(dir);
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(reader->max_doc().value(), 3);
    EXPECT_EQ(reader->num_docs().value(), 2);
    EXPECT_EQ(hits_of(reader, kTitleField, "two").size(), 0U);
    EXPECT_EQ(hits_of(reader, kTitleField, "one").size(), 1U);
    EXPECT_TRUE(writer.value()->has_deletions().value());
}

TEST(IndexWriterTest, UpdatesADocument) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    // These assertions are about delete bookkeeping, which a merge would
    // reclaim, so merging is turned off.
    config.set_merge_policy(NoMergePolicy::create());

    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("a", "old title")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    ASSERT_TRUE(writer.value()->update_document(id_term("a"), make_doc("a", "new title")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto reader = open_reader(dir);
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(reader->num_docs().value(), 1);
    EXPECT_EQ(hits_of(reader, kTitleField, "old").size(), 0U);
    EXPECT_EQ(hits_of(reader, kTitleField, "new").size(), 1U);
}

TEST(IndexWriterTest, RollbackDropsUncommittedWork) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);

    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("a", "kept")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    ASSERT_TRUE(writer.value()->add_document(make_doc("b", "dropped")).has_value());
    ASSERT_TRUE(writer.value()->rollback().has_value());
    EXPECT_EQ(writer.value()->num_docs().value(), 1);

    auto reader = open_reader(dir);
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(reader->num_docs().value(), 1);
    EXPECT_EQ(hits_of(reader, kTitleField, "dropped").size(), 0U);

    // The writer stays usable after a rollback.
    ASSERT_TRUE(writer.value()->add_document(make_doc("c", "after")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    auto reopened = open_reader(dir);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(reopened->num_docs().value(), 2);
}

TEST(IndexWriterTest, DeleteAllEmptiesTheIndex) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);

    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("a", "one")).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("b", "two")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    ASSERT_TRUE(writer.value()->delete_all().has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    EXPECT_EQ(writer.value()->num_docs().value(), 0);

    auto reader = open_reader(dir);
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(reader->num_docs().value(), 0);
    EXPECT_EQ(hits_of(reader, kTitleField, "one").size(), 0U);
}

TEST(IndexWriterTest, CloseCommitsAndRejectsFurtherWrites) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);

    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("a", "one")).has_value());
    ASSERT_TRUE(writer.value()->close().has_value());
    EXPECT_FALSE(writer.value()->is_open());

    auto rejected = writer.value()->add_document(make_doc("b", "two"));
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().kind(), ErrorKind::kAlreadyClosed);

    auto reader = open_reader(dir);
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(reader->num_docs().value(), 1);
}
