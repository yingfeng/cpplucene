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

// The flush queue: a flushed segment waits in the writer until the next commit (or
// near-real-time read) publishes it, which is what the reference implementation's
// `DocumentsWriterFlushQueue` counts as tickets.

module;

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

module lucene_core:ut.flush_queue;

import lucene_core;

using namespace lucene;

namespace {

FieldType body_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    return type;
}

void add_documents(IndexWriter &writer, int32_t count) {
    for (int32_t i = 0; i < count; ++i) {
        Document document;
        document.add(std::make_unique<Field>("body", FieldValue{std::string("alpha")}, body_type()));
        ASSERT_TRUE(writer.add_document(std::move(document)).has_value());
    }
}

} // namespace

TEST(FlushQueueTest, AFlushedSegmentWaitsForTheCommit) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_max_buffered_docs(2);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());

    // One document: nothing was flushed, so the queue is empty.
    add_documents(*writer.value(), 1);
    EXPECT_EQ(writer.value()->flush_count(), 0);
    EXPECT_EQ(writer.value()->pending_flush_count(), 0U);

    // Five documents with a threshold of two: two segments were flushed and are
    // waiting in the queue, the rest is still buffered.
    add_documents(*writer.value(), 4);
    EXPECT_EQ(writer.value()->flush_count(), 2);
    EXPECT_EQ(writer.value()->pending_flush_count(), 2U);
    EXPECT_TRUE(writer.value()->has_uncommitted_changes());

    // A near-real-time read publishes the queue.
    auto nrt = writer.value()->get_reader();
    ASSERT_TRUE(nrt.has_value()) << (nrt.has_value() ? "" : nrt.error().to_string());
    EXPECT_EQ(writer.value()->pending_flush_count(), 0U);
    IndexSearcher nrt_searcher(nrt.value());
    auto nrt_count = nrt_searcher.count(MatchAllDocsQuery());
    ASSERT_TRUE(nrt_count.has_value()) << nrt_count.error().to_string();
    EXPECT_EQ(nrt_count.value(), 5);
}

TEST(FlushQueueTest, ACommitPublishesTheQueue) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_max_buffered_docs(2);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value());

    add_documents(*writer.value(), 5);
    ASSERT_EQ(writer.value()->pending_flush_count(), 2U);

    ASSERT_TRUE(writer.value()->commit().has_value());
    EXPECT_EQ(writer.value()->pending_flush_count(), 0U);
    EXPECT_FALSE(writer.value()->has_uncommitted_changes());
    ASSERT_TRUE(writer.value()->close().has_value());

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->num_docs().value(), 5);
}

TEST(FlushQueueTest, ARollbackDropsTheQueue) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_max_buffered_docs(2);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value());

    add_documents(*writer.value(), 5);
    ASSERT_EQ(writer.value()->pending_flush_count(), 2U);

    ASSERT_TRUE(writer.value()->rollback().has_value());
    // The writer is closed now, and the flushed segments were never committed.
    auto reader = DirectoryReader::open(directory);
    if (reader.has_value()) {
        EXPECT_EQ(reader.value()->num_docs().value(), 0);
    }
}
