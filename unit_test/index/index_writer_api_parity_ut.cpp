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

// `IndexWriter::get_doc_stats` and the batch update by term: the counts a caller
// reads while writing, and the "replace all versions of these ids" call.

module;

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.index_writer_api_parity;

import lucene_core;

using namespace lucene;

namespace {

FieldType id_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::Docs).has_value());
    return type;
}

FieldType body_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

Document make_document(std::string id, std::string body) {
    Document document;
    document.add(std::make_unique<Field>("id", FieldValue{std::move(id)}, id_type()));
    document.add(std::make_unique<Field>("body", FieldValue{std::move(body)}, body_type()));
    return document;
}

int32_t count_of(IndexSearcher &searcher, const Query &query) {
    auto counted = searcher.count(query);
    EXPECT_TRUE(counted.has_value()) << (counted.has_value() ? "" : counted.error().to_string());
    return counted.has_value() ? counted.value() : -1;
}

} // namespace

TEST(IndexWriterApiParityTest, DocStatsCountTheBufferedDocumentsToo) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_max_buffered_docs(2);
    // Without a merge the deleted document keeps its slot, so `max_doc` stays 3.
    config.set_merge_policy(NoMergePolicy::create());
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());

    // Two documents are buffered, nothing is committed yet.
    ASSERT_TRUE(writer.value()->add_document(make_document("a", "one")).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_document("b", "two")).has_value());
    DocStats buffered = writer.value()->get_doc_stats();
    EXPECT_EQ(buffered.num_docs, 2);
    EXPECT_EQ(buffered.max_doc, 2);

    // The third document flushes the first two: the counts do not change.
    ASSERT_TRUE(writer.value()->add_document(make_document("c", "three")).has_value());
    DocStats flushed = writer.value()->get_doc_stats();
    EXPECT_EQ(flushed.num_docs, 3);
    EXPECT_EQ(flushed.max_doc, 3);
    EXPECT_EQ(writer.value()->pending_flush_count(), 1U);

    // A delete reaches the documents of the queue, so the live count drops but the
    // total stays.
    ASSERT_TRUE(writer.value()->delete_documents(Term::from_text("id", "a")).has_value());
    DocStats deleted = writer.value()->get_doc_stats();
    EXPECT_EQ(deleted.max_doc, 3);
    ASSERT_TRUE(writer.value()->commit().has_value());
    DocStats committed = writer.value()->get_doc_stats();
    EXPECT_EQ(committed.num_docs, 2);
    EXPECT_EQ(committed.max_doc, 3);
}

TEST(IndexWriterApiParityTest, UpdatingByTermReplacesEveryVersionInOneCall) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value());

    // Two documents share the id "x", a third has its own id.
    ASSERT_TRUE(writer.value()->add_document(make_document("x", "old-one")).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_document("x", "old-two")).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_document("y", "other")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    // One call replaces both versions of "x".
    std::vector<std::unique_ptr<Document>> documents;
    documents.push_back(std::make_unique<Document>(make_document("x", "new")));
    std::vector<const Document *> borrowed{documents.front().get()};
    ASSERT_TRUE(writer.value()->update_documents(Term::from_text("id", "x"), borrowed).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    IndexSearcher searcher(reader.value());
    EXPECT_EQ(count_of(searcher, MatchAllDocsQuery()), 2); // "x"/new and "y"
    TermQuery old_one(Term::from_text("body", "old-one"));
    TermQuery old_two(Term::from_text("body", "old-two"));
    TermQuery fresh(Term::from_text("body", "new"));
    TermQuery other(Term::from_text("body", "other"));
    EXPECT_EQ(count_of(searcher, old_one), 0);
    EXPECT_EQ(count_of(searcher, old_two), 0);
    EXPECT_EQ(count_of(searcher, fresh), 1);
    EXPECT_EQ(count_of(searcher, other), 1);
}

TEST(IndexWriterApiParityTest, ABatchWithANullDocumentIsRejected) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value());

    std::vector<const Document *> documents{nullptr};
    auto updated = writer.value()->update_documents(Term::from_text("id", "x"), documents);
    ASSERT_FALSE(updated.has_value());
    EXPECT_EQ(updated.error().kind(), ErrorKind::kIllegalArgument);
    // Nothing was buffered by the rejected call.
    EXPECT_FALSE(writer.value()->has_uncommitted_changes());
}

TEST(IndexWriterApiParityTest, TheOpenModeDecidesWhetherAnIndexMayExist) {
    ByteBuffersDirectory directory;

    // Appending to an empty directory is an error.
    {
        WhitespaceAnalyzer analyzer;
        IndexWriterConfig config(analyzer);
        config.set_open_mode(OpenMode::Append);
        auto writer = IndexWriter::open(directory, config);
        ASSERT_FALSE(writer.has_value());
        EXPECT_EQ(writer.error().kind(), ErrorKind::kIndexNotFound);
    }

    // Creating on an empty directory works, and creating again is an error.
    {
        WhitespaceAnalyzer analyzer;
        IndexWriterConfig create(analyzer);
        create.set_open_mode(OpenMode::Create);
        auto writer = IndexWriter::open(directory, create);
        ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());
        ASSERT_TRUE(writer.value()->add_document(make_document("a", "one")).has_value());
        ASSERT_TRUE(writer.value()->close().has_value());

        IndexWriterConfig again(analyzer);
        again.set_open_mode(OpenMode::Create);
        auto second = IndexWriter::open(directory, again);
        ASSERT_FALSE(second.has_value());
        EXPECT_EQ(second.error().kind(), ErrorKind::kIllegalState);
    }

    // Appending keeps what the index holds.
    {
        WhitespaceAnalyzer analyzer;
        IndexWriterConfig config(analyzer);
        config.set_open_mode(OpenMode::Append);
        auto writer = IndexWriter::open(directory, config);
        ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());
        ASSERT_TRUE(writer.value()->add_document(make_document("b", "two")).has_value());
        ASSERT_TRUE(writer.value()->close().has_value());
    }

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->num_docs().value(), 2);
}

TEST(IndexWriterApiParityTest, ClosingWithoutCommitDropsTheBufferedChanges) {
    ByteBuffersDirectory directory;
    {
        WhitespaceAnalyzer analyzer;
        IndexWriterConfig config(analyzer);
        config.set_commit_on_close(false);
        auto writer = IndexWriter::open(directory, config);
        ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());
        ASSERT_TRUE(writer.value()->add_document(make_document("a", "one")).has_value());
        // The document stays buffered and is dropped by the close.
        ASSERT_TRUE(writer.value()->close().has_value());
    }

    auto reader = DirectoryReader::open(directory);
    if (reader.has_value()) {
        EXPECT_EQ(reader.value()->num_docs().value(), 0);
    }

    // With the default the close commits, so the same sequence keeps the document.
    {
        WhitespaceAnalyzer analyzer;
        IndexWriterConfig config(analyzer);
        auto writer = IndexWriter::open(directory, config);
        ASSERT_TRUE(writer.has_value());
        ASSERT_TRUE(writer.value()->add_document(make_document("b", "two")).has_value());
        ASSERT_TRUE(writer.value()->close().has_value());
    }
    auto reopened = DirectoryReader::open(directory);
    ASSERT_TRUE(reopened.has_value()) << (reopened.has_value() ? "" : reopened.error().to_string());
    EXPECT_EQ(reopened.value()->num_docs().value(), 1);
}
