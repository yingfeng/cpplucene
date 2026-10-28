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

module lucene_core:ut.concurrent_merge_scheduler;

import lucene_core;

using namespace lucene;

namespace {

FieldType body_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

Document make_doc(std::string id, std::string body) {
    Document doc;
    doc.add(std::make_unique<Field>("id", FieldValue{std::move(id)}, body_type()));
    doc.add(std::make_unique<Field>("body", FieldValue{std::move(body)}, body_type()));
    return doc;
}

/// A configuration that flushes often and merges eagerly, so the background
/// scheduler really has merges to run.
IndexWriterConfig scheduler_config(WhitespaceAnalyzer &analyzer, int32_t threads) {
    IndexWriterConfig config(analyzer);
    config.set_merge_scheduler(ConcurrentMergeScheduler::create(threads));
    config.set_ram_buffer_size_mb(1);
    auto policy = TieredMergePolicy::create();
    EXPECT_TRUE(policy->set_segments_per_tier(2.0).has_value());
    EXPECT_TRUE(policy->set_max_merge_at_once(3).has_value());
    config.set_merge_policy(std::shared_ptr<MergePolicy>(std::move(policy)));
    return config;
}

/// A body wide enough that a 1 MB buffer flushes every couple of hundred
/// documents.
std::string wide_body() { return std::string(2000, 'x') + " alpha beta gamma"; }

} // namespace

TEST(ConcurrentMergeSchedulerTest, MergesRunInTheBackgroundAndTheWriterStaysConsistent) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config = scheduler_config(analyzer, 2);
    auto scheduler = std::dynamic_pointer_cast<ConcurrentMergeScheduler>(config.merge_scheduler());
    ASSERT_NE(scheduler, nullptr);
    EXPECT_EQ(scheduler->to_string(), "ConcurrentMergeScheduler");
    EXPECT_EQ(scheduler->max_thread_count(), 2);

    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    for (int32_t i = 0; i < 2000; ++i) {
        // The merges this triggers run on the scheduler's workers while the loop
        // keeps adding documents.
        ASSERT_TRUE(writer.value()->add_document(make_doc("d" + std::to_string(i), wide_body())).has_value())
                << "document " << i;
    }
    ASSERT_TRUE(writer.value()->commit().has_value());
    EXPECT_EQ(scheduler->pending_merge_count(), 0U) << "commit waits for the merges it started";
    EXPECT_LE(writer.value()->segment_count(), 4U) << "the merges actually happened";

    auto reader = writer.value()->get_reader();
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->num_docs().value(), 2000);

    IndexSearcher searcher(reader.value());
    // `total_hits` of a top-n search is only a lower bound once it passes the
    // threshold, so the exact count comes from `count`.
    auto hits = searcher.search(TermQuery(Term::from_text("body", "beta")), 5);
    ASSERT_TRUE(hits.has_value());
    EXPECT_EQ(hits.value().total_hits.relation, TotalHits::Relation::GreaterThanOrEqualTo);
    EXPECT_GE(hits.value().total_hits.value, 1000);
    auto counted = searcher.count(TermQuery(Term::from_text("body", "beta")));
    ASSERT_TRUE(counted.has_value());
    EXPECT_EQ(counted.value(), 2000);
    ASSERT_TRUE(writer.value()->close().has_value());
}

TEST(ConcurrentMergeSchedulerTest, ForceMergeWaitsForTheResult) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config = scheduler_config(analyzer, 2);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    for (int32_t i = 0; i < 300; ++i) {
        ASSERT_TRUE(writer.value()->add_document(make_doc("d" + std::to_string(i), "alpha")).has_value());
    }
    auto merged = writer.value()->force_merge(1);
    ASSERT_TRUE(merged.has_value()) << (merged ? "" : merged.error().to_string());
    EXPECT_EQ(writer.value()->segment_count(), 1U) << "force_merge is synchronous";

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->num_docs().value(), 300);
    ASSERT_TRUE(writer.value()->close().has_value());
}

TEST(ConcurrentMergeSchedulerTest, SeveralIndexingThreadsShareOneWriter) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config = scheduler_config(analyzer, 2);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    constexpr int32_t kThreads = 4;
    constexpr int32_t kPerThread = 150;
    std::vector<std::thread> threads;
    std::vector<int32_t> failures(kThreads, 0);
    threads.reserve(kThreads);
    for (int32_t t = 0; t < kThreads; ++t) {
        threads.emplace_back([&writer, &failures, t] {
            for (int32_t i = 0; i < kPerThread; ++i) {
                const std::string id = "t" + std::to_string(t) + "-" + std::to_string(i);
                if (!writer.value()->add_document(make_doc(id, "alpha beta")).has_value()) {
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
    ASSERT_TRUE(writer.value()->force_merge(1).has_value());
    ASSERT_TRUE(writer.value()->close().has_value());

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->num_docs().value(), kThreads * kPerThread);
    IndexSearcher searcher(reader.value());
    auto hits = searcher.search(TermQuery(Term::from_text("body", "beta")), 1);
    ASSERT_TRUE(hits.has_value());
    EXPECT_EQ(hits.value().total_hits.value, kThreads * kPerThread);
}

TEST(ConcurrentMergeSchedulerTest, DeletesAndUpdatesSurviveTheBackgroundMerges) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config = scheduler_config(analyzer, 2);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    for (int32_t i = 0; i < 400; ++i) {
        ASSERT_TRUE(writer.value()->add_document(make_doc("d" + std::to_string(i), "alpha")).has_value());
    }
    for (int32_t i = 0; i < 400; i += 2) {
        ASSERT_TRUE(writer.value()->delete_documents(Term::from_text("id", "d" + std::to_string(i))).has_value());
    }
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->force_merge(1).has_value());
    ASSERT_TRUE(writer.value()->close().has_value());

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    // Only the odd documents survive, and the merge had to drop the deleted ones.
    EXPECT_EQ(reader.value()->num_docs().value(), 200);
    IndexSearcher searcher(reader.value());
    auto hits = searcher.search(TermQuery(Term::from_text("id", "d1")), 5);
    ASSERT_TRUE(hits.has_value());
    EXPECT_EQ(hits.value().score_docs.size(), 1U);
    auto gone = searcher.search(TermQuery(Term::from_text("id", "d2")), 5);
    ASSERT_TRUE(gone.has_value());
    EXPECT_EQ(gone.value().score_docs.size(), 0U);
}

TEST(ConcurrentMergeSchedulerTest, ASchedulerCannotBeSharedBetweenWriters) {
    ByteBuffersDirectory first;
    ByteBuffersDirectory second;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config = scheduler_config(analyzer, 1);

    auto first_writer = IndexWriter::open(first, config);
    ASSERT_TRUE(first_writer.has_value()) << (first_writer ? "" : first_writer.error().to_string());
    // Enough documents to make the writer flush and merge, which is what binds
    // the scheduler to this writer.
    for (int32_t i = 0; i < 2000; ++i) {
        ASSERT_TRUE(first_writer.value()->add_document(make_doc("a" + std::to_string(i), wide_body())).has_value());
    }
    ASSERT_TRUE(first_writer.value()->commit().has_value());
    auto second_writer = IndexWriter::open(second, config);
    ASSERT_TRUE(second_writer.has_value()) << (second_writer ? "" : second_writer.error().to_string());
    for (int32_t i = 0; i < 2000; ++i) {
        ASSERT_TRUE(second_writer.value()->add_document(make_doc("b" + std::to_string(i), wide_body())).has_value());
    }
    // The second writer's merges are rejected: the scheduler belongs to the first.
    // A commit is what asks the scheduler for merges (a forced merge runs inline).
    const VoidResult merged = second_writer.value()->commit();
    EXPECT_FALSE(merged.has_value());
    if (!merged.has_value()) {
        EXPECT_NE(merged.error().to_string().find("shared"), std::string::npos) << merged.error().to_string();
    }
    ASSERT_TRUE(second_writer.value()->close().has_value());
    ASSERT_TRUE(first_writer.value()->close().has_value());
}

TEST(ConcurrentMergeSchedulerTest, ThreadCountIsValidated) {
    auto scheduler = ConcurrentMergeScheduler::create(0);
    EXPECT_EQ(scheduler->max_thread_count(), 1) << "a pool needs at least one worker";
    EXPECT_FALSE(scheduler->set_max_thread_count(0).has_value());
    EXPECT_TRUE(scheduler->set_max_thread_count(4).has_value());
    EXPECT_EQ(scheduler->max_thread_count(), 4);
    EXPECT_TRUE(scheduler->wait_for_merges().has_value());
    EXPECT_TRUE(scheduler->close().has_value());
}
