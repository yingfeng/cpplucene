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

// Leaf slices, the task executor and a parallel search over several segments:
// the slices have to cover every leaf exactly once, the executor has to run each
// task exactly once and report the first failure, and a parallel search has to
// answer exactly what the serial one answers.

module;

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

module lucene_core:ut.parallel_search;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kBody = "body";
constexpr std::string_view kNum = "num";

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    return type;
}

/// Builds an index of `segments * per_segment` documents, one commit per segment.
void build_index(Directory &directory, int32_t segments, int32_t per_segment) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());

    int32_t doc = 0;
    for (int32_t segment = 0; segment < segments; ++segment) {
        for (int32_t i = 0; i < per_segment; ++i, ++doc) {
            Document document;
            document.add(std::make_unique<Field>(std::string(kBody),
                                                 FieldValue{std::string(doc % 2 == 0 ? "alpha" : "beta")},
                                                 text_type()));
            auto num = NumericDocValuesField::create(std::string(kNum), doc);
            ASSERT_TRUE(num.has_value());
            document.add(std::make_unique<Field>(std::move(num.value())));
            ASSERT_TRUE(writer.value()->add_document(std::move(document)).has_value());
        }
        ASSERT_TRUE(writer.value()->commit().has_value());
    }
    ASSERT_TRUE(writer.value()->close().has_value());
}

std::vector<int32_t> hit_docs(const TopDocs &top) {
    std::vector<int32_t> docs;
    for (const ScoreDoc &hit : top.score_docs) {
        docs.push_back(hit.doc);
    }
    return docs;
}

/// The number of documents the slices cover, by leaf ordinal.
std::vector<int32_t> leaf_ords_of(const std::vector<LeafSlice> &slices) {
    std::vector<int32_t> ords;
    for (const LeafSlice &slice : slices) {
        ords.insert(ords.end(), slice.leaves.begin(), slice.leaves.end());
    }
    return ords;
}

} // namespace

// ---------------------------------------------------------------------------
// leaf slices
// ---------------------------------------------------------------------------

TEST(LeafSliceTest, OneSegmentPerSliceCoversEveryLeafOnce) {
    ByteBuffersDirectory directory;
    build_index(directory, 12, 1);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    IndexSearcher searcher(reader.value());
    const auto leaves = searcher.leaves();
    ASSERT_TRUE(leaves.has_value());
    const size_t leaf_count = leaves.value().size();
    ASSERT_GE(leaf_count, 2U); // the writer may merge, but not everything

    auto slices = slices_with_limits(leaves.value(), 1'000, 1);
    ASSERT_TRUE(slices.has_value()) << slices.error().to_string();
    ASSERT_EQ(slices.value().size(), leaf_count);
    std::vector<int32_t> ords;
    for (const LeafSlice &slice : slices.value()) {
        ASSERT_EQ(slice.leaves.size(), 1U);
        ords.push_back(slice.leaves.front());
    }
    std::vector<int32_t> expected(leaf_count);
    for (size_t i = 0; i < leaf_count; ++i) {
        expected[i] = static_cast<int32_t>(i);
    }
    EXPECT_EQ(ords, expected);
}

TEST(LeafSliceTest, EveryLeafIsInExactlyOneSlice) {
    ByteBuffersDirectory directory;
    build_index(directory, 12, 1);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    IndexSearcher searcher(reader.value());
    const auto leaves = searcher.leaves();
    ASSERT_TRUE(leaves.has_value());

    auto result = slices(leaves.value());
    ASSERT_TRUE(result.has_value()) << result.error().to_string();
    ASSERT_FALSE(result.value().empty());
    std::vector<int32_t> ords = leaf_ords_of(result.value());
    std::vector<int32_t> unique = ords;
    std::ranges::sort(unique);
    const auto duplicate = std::ranges::adjacent_find(unique);
    EXPECT_EQ(duplicate, unique.end()) << "a leaf is in two slices";
    EXPECT_EQ(unique.size(), leaves.value().size());
    for (const LeafSlice &slice : result.value()) {
        EXPECT_LE(slice.leaves.size(), MAX_SEGMENTS_PER_SLICE);
        EXPECT_FALSE(slice.leaves.empty());
        // The leaves of a slice are visited in document order.
        EXPECT_TRUE(std::ranges::is_sorted(slice.leaves));
    }
}

TEST(LeafSliceTest, SlicesKeepTheDocumentCountBounded) {
    ByteBuffersDirectory directory;
    build_index(directory, 12, 1);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    IndexSearcher searcher(reader.value());
    const auto leaves = searcher.leaves();
    ASSERT_TRUE(leaves.has_value());

    int32_t max_docs_per_slice = 0;
    int32_t biggest_leaf = 0;
    for (const LeafReaderContext &leaf : leaves.value()) {
        const int32_t max_doc = leaf.reader()->max_doc().value();
        max_docs_per_slice += max_doc;
        biggest_leaf = std::max(biggest_leaf, max_doc);
    }
    // Ask for slices of about one segment's worth of documents.
    max_docs_per_slice = std::max(1, max_docs_per_slice / static_cast<int32_t>(leaves.value().size()));
    auto result = slices_with_limits(leaves.value(), max_docs_per_slice, 5);
    ASSERT_TRUE(result.has_value()) << result.error().to_string();
    for (const LeafSlice &slice : result.value()) {
        int32_t documents = 0;
        for (const int32_t ord : slice.leaves) {
            documents += leaves.value()[static_cast<size_t>(ord)].reader()->max_doc().value();
        }
        // A slice is closed as soon as it goes over the limit, so the last leaf
        // it took may push it up to one leaf past the limit.
        EXPECT_LE(documents, max_docs_per_slice + biggest_leaf);
    }
}

TEST(LeafSliceTest, RejectsNonPositiveLimits) {
    ByteBuffersDirectory directory;
    build_index(directory, 1, 1);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());
    const auto leaves = searcher.leaves();
    ASSERT_TRUE(leaves.has_value());

    auto no_docs = slices_with_limits(leaves.value(), 0, 5);
    ASSERT_FALSE(no_docs.has_value());
    EXPECT_EQ(no_docs.error().kind(), ErrorKind::kIllegalArgument);
    auto no_segments = slices_with_limits(leaves.value(), 100, 0);
    ASSERT_FALSE(no_segments.has_value());
    EXPECT_EQ(no_segments.error().kind(), ErrorKind::kIllegalArgument);
}

// ---------------------------------------------------------------------------
// task executor
// ---------------------------------------------------------------------------

TEST(TaskExecutorTest, DirectRunsTheTasksInOrderOnTheCallingThread) {
    const std::shared_ptr<TaskExecutor> executor = TaskExecutor::direct();
    EXPECT_TRUE(executor->is_direct());
    EXPECT_EQ(executor->parallelism(), 1U);

    std::vector<std::function<Result<int32_t>()>> tasks;
    std::vector<std::thread::id> threads;
    for (int32_t i = 0; i < 4; ++i) {
        tasks.push_back([i, &threads]() -> Result<int32_t> {
            threads.push_back(std::this_thread::get_id());
            return i * 10;
        });
    }
    auto results = executor->invoke_all<int32_t>(std::move(tasks));
    ASSERT_TRUE(results.has_value()) << results.error().to_string();
    EXPECT_EQ(results.value(), (std::vector<int32_t>{0, 10, 20, 30}));
    for (const std::thread::id &thread : threads) {
        EXPECT_EQ(thread, std::this_thread::get_id());
    }
}

TEST(TaskExecutorTest, ThreadsRunEveryTaskExactlyOnce) {
    auto executor = TaskExecutor::with_threads(4);
    ASSERT_TRUE(executor.has_value()) << executor.error().to_string();
    EXPECT_EQ(executor.value()->parallelism(), 4U);

    std::atomic<int32_t> in_flight{0};
    std::atomic<int32_t> peak{0};
    std::atomic<int32_t> calls{0};
    std::vector<std::function<Result<int32_t>()>> tasks;
    for (int32_t i = 0; i < 16; ++i) {
        tasks.push_back([i, &in_flight, &peak, &calls]() -> Result<int32_t> {
            calls.fetch_add(1);
            const int32_t running = in_flight.fetch_add(1) + 1;
            int32_t observed = peak.load();
            while (observed < running && !peak.compare_exchange_weak(observed, running)) {
                // retry with the value compare_exchange_weak left behind
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            in_flight.fetch_sub(1);
            return i;
        });
    }
    auto results = executor.value()->invoke_all<int32_t>(std::move(tasks));
    ASSERT_TRUE(results.has_value()) << results.error().to_string();
    ASSERT_EQ(results.value().size(), 16U);
    for (int32_t i = 0; i < 16; ++i) {
        EXPECT_EQ(results.value()[static_cast<size_t>(i)], i);
    }
    EXPECT_EQ(calls.load(), 16);
    EXPECT_GE(peak.load(), 2);
}

TEST(TaskExecutorTest, ReportsTheFirstFailureInTaskOrder) {
    auto executor = TaskExecutor::with_threads(3);
    ASSERT_TRUE(executor.has_value());
    std::vector<std::function<Result<int32_t>()>> tasks;
    for (int32_t i = 0; i < 6; ++i) {
        tasks.push_back([i]() -> Result<int32_t> {
            if (i == 1 || i == 4) {
                return err(LuceneError::illegal_state("task " + std::to_string(i) + " failed"));
            }
            return i;
        });
    }
    auto results = executor.value()->invoke_all<int32_t>(std::move(tasks));
    ASSERT_FALSE(results.has_value());
    EXPECT_EQ(results.error().kind(), ErrorKind::kIllegalState);
    EXPECT_NE(results.error().to_string().find("task 1 failed"), std::string::npos);
}

TEST(TaskExecutorTest, RunsMoveOnlyTasks) {
    auto executor = TaskExecutor::with_threads(2);
    ASSERT_TRUE(executor.has_value());

    // A task owns its value, which is what a slice task does with its collector.
    auto make_task = [](std::unique_ptr<int32_t> value) {
        return [value = std::move(value)]() -> Result<std::unique_ptr<int32_t>> {
            return std::unique_ptr<int32_t>(new int32_t(*value + 1));
        };
    };
    using Task = decltype(make_task(std::unique_ptr<int32_t>{}));
    std::vector<Task> tasks;
    for (int32_t i = 0; i < 4; ++i) {
        tasks.push_back(make_task(std::make_unique<int32_t>(i)));
    }
    auto results = executor.value()->invoke_all<std::unique_ptr<int32_t>>(std::move(tasks));
    ASSERT_TRUE(results.has_value()) << results.error().to_string();
    ASSERT_EQ(results.value().size(), 4U);
    for (int32_t i = 0; i < 4; ++i) {
        ASSERT_NE(results.value()[static_cast<size_t>(i)], nullptr);
        EXPECT_EQ(*results.value()[static_cast<size_t>(i)], i + 1);
    }
}

// ---------------------------------------------------------------------------
// parallel search
// ---------------------------------------------------------------------------

TEST(ParallelSearchTest, AnswersExactlyWhatTheSerialOneDoes) {
    ByteBuffersDirectory directory;
    build_index(directory, 6, 10); // six segments, sixty documents
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    ASSERT_EQ(reader.value()->num_docs().value(), 60);

    const IndexSearcher serial(reader.value());
    auto parallel = IndexSearcher::with_executor(reader.value(), nullptr, 4);
    ASSERT_TRUE(parallel.has_value()) << parallel.error().to_string();
    EXPECT_EQ(parallel.value()->task_executor().parallelism(), 4U);
    EXPECT_TRUE(serial.task_executor().is_direct());

    auto alpha = TopScoreDocCollectorManager::create(5, IndexSearcher::TOTAL_HITS_THRESHOLD);
    ASSERT_TRUE(alpha.has_value());
    auto everything =
            TopScoreDocCollectorManager::create(10, IndexSearcher::TOTAL_HITS_THRESHOLD);
    ASSERT_TRUE(everything.has_value());

    // A term query: the same five hits, in the same order, with the same total.
    const TermQuery alpha_query(Term::from_text(std::string(kBody), "alpha"));
    auto serial_alpha = serial.search_with_collector_manager(alpha_query, *alpha.value());
    ASSERT_TRUE(serial_alpha.has_value()) << serial_alpha.error().to_string();
    auto parallel_alpha = parallel.value()->search_with_collector_manager(alpha_query, *alpha.value());
    ASSERT_TRUE(parallel_alpha.has_value()) << parallel_alpha.error().to_string();
    EXPECT_EQ(hit_docs(parallel_alpha.value()), hit_docs(serial_alpha.value()));
    EXPECT_EQ(parallel_alpha.value().total_hits.value, serial_alpha.value().total_hits.value);
    EXPECT_EQ(serial_alpha.value().total_hits.value, 30);

    // Match all: every document, and the total is the document count.
    auto serial_all = serial.search_with_collector_manager(MatchAllDocsQuery(), *everything.value());
    ASSERT_TRUE(serial_all.has_value()) << serial_all.error().to_string();
    auto parallel_all =
            parallel.value()->search_with_collector_manager(MatchAllDocsQuery(), *everything.value());
    ASSERT_TRUE(parallel_all.has_value()) << parallel_all.error().to_string();
    EXPECT_EQ(hit_docs(parallel_all.value()), hit_docs(serial_all.value()));
    EXPECT_EQ(parallel_all.value().total_hits.value, 60);

    // A boolean query with a must-not clause, over the same segments.
    BooleanQuery::Builder builder;
    ASSERT_TRUE(builder.add(std::make_shared<TermQuery>(Term::from_text(std::string(kBody), "alpha")), Occur::Must)
                        .has_value());
    ASSERT_TRUE(builder
                        .add(std::make_shared<TermQuery>(Term::from_text(std::string(kBody), "beta")), Occur::MustNot)
                        .has_value());
    auto boolean = builder.build();
    ASSERT_TRUE(boolean.has_value()) << boolean.error().to_string();
    auto serial_boolean = serial.search_with_collector_manager(boolean.value(), *everything.value());
    ASSERT_TRUE(serial_boolean.has_value()) << serial_boolean.error().to_string();
    auto parallel_boolean =
            parallel.value()->search_with_collector_manager(boolean.value(), *everything.value());
    ASSERT_TRUE(parallel_boolean.has_value()) << parallel_boolean.error().to_string();
    EXPECT_EQ(hit_docs(parallel_boolean.value()), hit_docs(serial_boolean.value()));
    EXPECT_EQ(parallel_boolean.value().total_hits.value, 30);
    EXPECT_EQ(serial_boolean.value().total_hits.value, 30);

    // The scoreless paths still work on the parallel searcher: counting visits
    // every leaf once, and paging walks the hits without repeating one.
    auto serial_count = serial.count(MatchAllDocsQuery());
    ASSERT_TRUE(serial_count.has_value()) << serial_count.error().to_string();
    auto parallel_count = parallel.value()->count(MatchAllDocsQuery());
    ASSERT_TRUE(parallel_count.has_value()) << parallel_count.error().to_string();
    EXPECT_EQ(parallel_count.value(), serial_count.value());
    EXPECT_EQ(parallel_count.value(), 60);

    auto first = parallel.value()->search_after_score(nullptr, MatchAllDocsQuery(), 5);
    ASSERT_TRUE(first.has_value()) << first.error().to_string();
    auto second = parallel.value()->search_after_score(&first.value().score_docs.back(), MatchAllDocsQuery(), 5);
    ASSERT_TRUE(second.has_value()) << second.error().to_string();
    ASSERT_EQ(first.value().score_docs.size(), 5U);
    ASSERT_EQ(second.value().score_docs.size(), 5U);
    // Every score is the same, so the pages come in document order and the
    // second page starts after the last hit of the first one.
    EXPECT_LT(first.value().score_docs.back().doc, second.value().score_docs.front().doc);
}
