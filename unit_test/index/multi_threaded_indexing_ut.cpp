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
#include <thread>
#include <vector>

module lucene_core:ut.multi_threaded_indexing;

import lucene_core;

using namespace lucene;

namespace {

FieldType body_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

FieldType id_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::Docs).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

Document make_doc(std::string id, std::string body) {
    Document doc;
    doc.add(std::make_unique<Field>("id", FieldValue{std::move(id)}, id_type()));
    doc.add(std::make_unique<Field>("body", FieldValue{std::move(body)}, body_type()));
    return doc;
}

IndexWriterConfig pool_config(WhitespaceAnalyzer &analyzer, size_t buffers) {
    IndexWriterConfig config(analyzer);
    config.set_max_indexing_buffers(buffers);
    config.set_merge_scheduler(NoMergeScheduler::create());
    // A small buffer makes the pool hand buffers out early.
    config.set_ram_buffer_size_mb(1);
    return config;
}

} // namespace

TEST(MultiThreadedIndexingTest, ThePoolSizeDefaultsToOne) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    EXPECT_EQ(config.max_indexing_buffers(), 1U) << "a single buffer keeps the documented flush thresholds";

    config.set_max_indexing_buffers(4);
    EXPECT_EQ(config.max_indexing_buffers(), 4U);
    config.set_max_indexing_buffers(0);
    EXPECT_EQ(config.max_indexing_buffers(), 1U) << "at least one buffer has to exist";
}

TEST(MultiThreadedIndexingTest, SeveralThreadsIndexIntoOneWriter) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config = pool_config(analyzer, 4);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    constexpr int32_t kThreads = 4;
    constexpr int32_t kPerThread = 400;
    std::vector<std::thread> threads;
    std::vector<int32_t> failures(kThreads, 0);
    threads.reserve(kThreads);
    for (int32_t t = 0; t < kThreads; ++t) {
        threads.emplace_back([&writer, &failures, t] {
            for (int32_t i = 0; i < kPerThread; ++i) {
                // Long bodies make the buffers fill up, so the threads really do
                // write segments while the others keep indexing.
                std::string body(400, 'x');
                body += " alpha beta";
                if (!writer.value()->add_document(make_doc("t" + std::to_string(t) + "-" + std::to_string(i), body))
                             .has_value()) {
                    ++failures[static_cast<size_t>(t)];
                }
            }
        });
    }
    for (std::thread &thread : threads) {
        thread.join();
    }
    for (const int32_t failed : failures) {
        EXPECT_EQ(failed, 0);
    }

    ASSERT_TRUE(writer.value()->commit().has_value());
    // The pool wrote more than one segment, which is what makes the writes parallel.
    EXPECT_GT(writer.value()->flush_count(), 1);

    auto reader = writer.value()->get_reader();
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->num_docs().value(), kThreads * kPerThread);

    IndexSearcher searcher(reader.value());
    // `total_hits` of a top-1 search is only a lower bound above the threshold, so
    // the exact number comes from `count`.
    auto counted_all = searcher.count(TermQuery(Term::from_text("body", "beta")));
    ASSERT_TRUE(counted_all.has_value());
    EXPECT_EQ(counted_all.value(), kThreads * kPerThread);
    auto counted = searcher.count(TermQuery(Term::from_text("id", "t1-7")));
    ASSERT_TRUE(counted.has_value());
    EXPECT_EQ(counted.value(), 1);
    ASSERT_TRUE(writer.value()->close().has_value());
}

TEST(MultiThreadedIndexingTest, EveryBufferCanAnalyzeOnItsOwn) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config = pool_config(analyzer, 4);
    // Without this factory the analysis of the shared analyzer is serialised; with
    // it every buffer brings its own and the analysis runs in parallel too.
    config.set_analyzer_provider([] { return std::shared_ptr<Analyzer>(std::make_shared<WhitespaceAnalyzer>()); });
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    constexpr int32_t kThreads = 4;
    constexpr int32_t kPerThread = 300;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int32_t t = 0; t < kThreads; ++t) {
        threads.emplace_back([&writer, t] {
            for (int32_t i = 0; i < kPerThread; ++i) {
                (void)writer.value()->add_document(
                        make_doc("t" + std::to_string(t) + "-" + std::to_string(i), "alpha beta gamma"));
            }
        });
    }
    for (std::thread &thread : threads) {
        thread.join();
    }
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->num_docs().value(), kThreads * kPerThread);
    IndexSearcher searcher(reader.value());
    auto counted = searcher.count(TermQuery(Term::from_text("body", "beta")));
    ASSERT_TRUE(counted.has_value());
    EXPECT_EQ(counted.value(), kThreads * kPerThread);
    ASSERT_TRUE(writer.value()->close().has_value());
}

TEST(MultiThreadedIndexingTest, FlushThreadsWriteTheBuffers) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config = pool_config(analyzer, 4);
    // Indexing hands the full buffers over; two workers write them, so no indexing
    // thread waits for a segment.
    config.set_flush_thread_count(2);
    config.set_analyzer_provider([] { return std::shared_ptr<Analyzer>(std::make_shared<WhitespaceAnalyzer>()); });
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    constexpr int32_t kThreads = 4;
    constexpr int32_t kPerThread = 400;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int32_t t = 0; t < kThreads; ++t) {
        threads.emplace_back([&writer, t] {
            for (int32_t i = 0; i < kPerThread; ++i) {
                std::string body(400, 'y');
                body += " alpha beta";
                (void)writer.value()->add_document(make_doc("t" + std::to_string(t) + "-" + std::to_string(i), body));
            }
        });
    }
    for (std::thread &thread : threads) {
        thread.join();
    }
    ASSERT_TRUE(writer.value()->commit().has_value());
    EXPECT_GT(writer.value()->flush_count(), 1) << "the workers wrote the segments";

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->num_docs().value(), kThreads * kPerThread);
    IndexSearcher searcher(reader.value());
    auto counted = searcher.count(TermQuery(Term::from_text("body", "beta")));
    ASSERT_TRUE(counted.has_value());
    EXPECT_EQ(counted.value(), kThreads * kPerThread);
    ASSERT_TRUE(writer.value()->close().has_value());
}

TEST(MultiThreadedIndexingTest, TheDeleteQueueNumbersItsEntries) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    DocumentsWriter writer(directory, config, 0);

    EXPECT_FALSE(writer.delete_queue().any());
    EXPECT_EQ(writer.delete_queue().size(), 0U);
    EXPECT_EQ(writer.delete_queue().max_seq_no(), 0);

    ASSERT_TRUE(writer.delete_documents(Term::from_text("id", "a"), 10).has_value());
    EXPECT_TRUE(writer.delete_queue().any());
    EXPECT_EQ(writer.delete_queue().size(), 1U);
    EXPECT_EQ(writer.delete_queue().max_seq_no(), 1);
    EXPECT_EQ(writer.delete_queue().entries().front().doc_upto, 10);

    ASSERT_TRUE(writer.soft_delete_documents(Term::from_text("id", "b"), 20).has_value());
    EXPECT_EQ(writer.delete_queue().size(), 2U);
    EXPECT_EQ(writer.delete_queue().max_seq_no(), 2);
    EXPECT_TRUE(writer.delete_queue().entries().back().soft);

    // The writer sees the same entries through the older accessor.
    EXPECT_EQ(writer.buffered_deletes().size(), 2U);
    writer.clear_delete_buffer();
    EXPECT_FALSE(writer.delete_queue().any());
    EXPECT_EQ(writer.delete_queue().max_seq_no(), 0);
}

TEST(MultiThreadedIndexingTest, ThePooledPathLosesNoDocument) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config = pool_config(analyzer, 2);
    // Two documents per buffer: the handoff happens constantly.
    config.set_max_buffered_docs(2);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    for (int32_t i = 0; i < 200; ++i) {
        ASSERT_TRUE(writer.value()->add_document(make_doc("d" + std::to_string(i), "alpha")).has_value())
                << "document " << i;
    }
    // The documents that sit in a buffer a thread is writing count as pending.
    EXPECT_EQ(writer.value()->num_docs().value(), 200);
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->num_docs().value(), 200);
    ASSERT_TRUE(writer.value()->close().has_value());
}

TEST(MultiThreadedIndexingTest, UpdatesAndDeletesSeeTheBufferedDocuments) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config = pool_config(analyzer, 4);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    for (int32_t i = 0; i < 100; ++i) {
        ASSERT_TRUE(writer.value()->add_document(make_doc("d" + std::to_string(i), "alpha")).has_value());
    }
    // Updating a document that is still buffered has to drop the old version, which
    // means the buffered documents have to be counted.
    ASSERT_TRUE(writer.value()->update_document(Term::from_text("id", "d7"), make_doc("d7", "beta")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->num_docs().value(), 100) << "the update replaced a document instead of adding one";
    IndexSearcher searcher(reader.value());
    auto updated = searcher.count(TermQuery(Term::from_text("body", "beta")));
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated.value(), 1);

    ASSERT_TRUE(writer.value()->close().has_value());
}

TEST(MultiThreadedIndexingTest, RollbackDropsTheSegmentsThePoolWrote) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config = pool_config(analyzer, 4);
    config.set_max_buffered_docs(2);
    config.set_commit_on_close(false);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    for (int32_t i = 0; i < 50; ++i) {
        ASSERT_TRUE(writer.value()->add_document(make_doc("d" + std::to_string(i), "alpha")).has_value());
    }
    ASSERT_TRUE(writer.value()->rollback().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());

    // Nothing was committed, so the index is empty (or does not exist at all).
    auto exists = DirectoryReader::index_exists(directory);
    ASSERT_TRUE(exists.has_value());
    if (exists.value()) {
        auto reader = DirectoryReader::open(directory);
        ASSERT_TRUE(reader.has_value());
        EXPECT_EQ(reader.value()->num_docs().value(), 0);
    }
}
