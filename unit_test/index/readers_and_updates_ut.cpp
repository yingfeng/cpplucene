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

#include <memory>
#include <string>

module lucene_core:ut.readers_and_updates;

import lucene_core;

using namespace lucene;

namespace {

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::Docs).has_value());
    return type;
}

Document make_document(const std::string &body) {
    Document doc;
    doc.add(std::make_unique<Field>(std::string("body"), FieldValue{body}, text_type()));
    return doc;
}

/// Writes a two document index with `NoMergePolicy`, so the segment keeps its
/// name and its deletions.
std::shared_ptr<IndexWriter> two_document_writer(ByteBuffersDirectory &dir, WhitespaceAnalyzer &analyzer) {
    IndexWriterConfig config(analyzer);
    config.set_merge_policy(std::make_unique<NoMergePolicy>());
    auto writer = IndexWriter::open(dir, config);
    EXPECT_TRUE(writer.has_value());
    if (!writer.has_value()) {
        return nullptr;
    }
    EXPECT_TRUE(writer.value()->add_document(make_document("alpha")).has_value());
    EXPECT_TRUE(writer.value()->add_document(make_document("beta")).has_value());
    EXPECT_TRUE(writer.value()->commit().has_value());
    return std::move(writer.value());
}

} // namespace

TEST(ReadersAndUpdatesTest, WrapsAReaderAndItsPendingDeletions) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    auto writer = two_document_writer(dir, analyzer);
    ASSERT_NE(writer, nullptr);
    SegmentCommitInfo segment = writer->segment_infos().segments.front();

    ReaderPool pool(dir);
    auto state = pool.get_readers_and_updates(segment, LUCENE_CURRENT_VERSION.major, io_context_default());
    ASSERT_TRUE(state.has_value()) << (state ? "" : state.error().to_string());
    EXPECT_NE(state.value()->reader(), nullptr);
    EXPECT_EQ(state.value()->reader()->num_docs().value(), 2);
    EXPECT_EQ(state.value()->pending_deletes().num_live_docs(), 2);
    EXPECT_EQ(state.value()->pending_deletes().num_pending_deletes(), 0);
    EXPECT_TRUE(state.value()->is_current_for(segment));
    EXPECT_FALSE(state.value()->buffered_updates().any());

    // The pool hands out the same state, and with it the same reader.
    auto again = pool.get_readers_and_updates(segment, LUCENE_CURRENT_VERSION.major, io_context_default());
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again.value(), state.value());
    EXPECT_EQ(pool.get(segment, LUCENE_CURRENT_VERSION.major, io_context_default()).value(),
              state.value()->reader());

    // Deleting a document only touches the pending state.
    ASSERT_TRUE(state.value()->pending_deletes().delete_doc(0).value());
    EXPECT_EQ(state.value()->pending_deletes().num_live_docs(), 1);
    EXPECT_EQ(state.value()->reader()->num_docs().value(), 2);
}

TEST(ReadersAndUpdatesTest, WritingTheLiveDocsMakesTheStateStale) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    auto writer = two_document_writer(dir, analyzer);
    ASSERT_NE(writer, nullptr);
    SegmentCommitInfo segment = writer->segment_infos().segments.front();

    ReaderPool pool(dir);
    auto state = pool.get_readers_and_updates(segment, LUCENE_CURRENT_VERSION.major, io_context_default());
    ASSERT_TRUE(state.has_value());
    ASSERT_TRUE(state.value()->pending_deletes().delete_doc(1).value());

    const auto &codec = lucene99_codec();
    auto written = state.value()->write_live_docs(codec.live_docs_format(), dir, segment, 1,
                                                 io_context_default());
    ASSERT_TRUE(written.has_value()) << (written ? "" : written.error().to_string());
    EXPECT_TRUE(written.value());

    // The cached reader still holds the old live docs, so the state must not be
    // handed out again: the pool reopens the segment instead.
    EXPECT_FALSE(state.value()->is_current_for(segment));
    segment.del_gen = segment.next_del_gen();
    segment.del_count = 1;
    EXPECT_FALSE(state.value()->is_current_for(segment));

    auto reopened = pool.get_readers_and_updates(segment, LUCENE_CURRENT_VERSION.major, io_context_default());
    ASSERT_TRUE(reopened.has_value());
    EXPECT_NE(reopened.value(), state.value());
    EXPECT_TRUE(reopened.value()->is_current_for(segment));
    EXPECT_EQ(reopened.value()->reader()->num_docs().value(), 1);
    EXPECT_EQ(reopened.value()->pending_deletes().num_live_docs(), 1);
    EXPECT_EQ(reopened.value()->pending_deletes().num_pending_deletes(), 0);

    ASSERT_TRUE(pool.close().has_value());
    ASSERT_TRUE(writer->close().has_value());
}

TEST(ReadersAndUpdatesTest, IsStaleWhenTheSegmentMovesOn) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    auto writer = two_document_writer(dir, analyzer);
    ASSERT_NE(writer, nullptr);
    SegmentCommitInfo segment = writer->segment_infos().segments.front();

    ReaderPool pool(dir);
    auto state = pool.get_readers_and_updates(segment, LUCENE_CURRENT_VERSION.major, io_context_default());
    ASSERT_TRUE(state.has_value());
    EXPECT_TRUE(state.value()->is_current_for(segment));

    // A doc values generation (or a field infos rewrite) makes the state stale.
    SegmentCommitInfo advanced = segment;
    ++advanced.doc_values_gen;
    EXPECT_FALSE(state.value()->is_current_for(advanced));

    SegmentCommitInfo grown = segment;
    grown.info.max_doc += 1;
    EXPECT_FALSE(state.value()->is_current_for(grown));

    // Dropping the segment forgets the state.
    pool.retain_only(SegmentInfos{});
    EXPECT_EQ(pool.num_cached(), 0U);
    EXPECT_EQ(pool.find(segment.info.name), nullptr);
  }
