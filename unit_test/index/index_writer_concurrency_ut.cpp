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

// Several threads sharing one `IndexWriter`: the writer-level lock has to
// serialize them, so every add, update and delete lands exactly once.

module;

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

module lucene_core:ut.index_writer_concurrency;

import lucene_core;

using namespace lucene;

namespace {

constexpr int32_t kThreads = 4;
constexpr int32_t kDocsPerThread = 50;
constexpr std::string_view kIdField = "id";
constexpr std::string_view kBodyField = "body";

FieldType id_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::Docs).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

FieldType body_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

Document make_document(int32_t id, const std::string &body) {
    Document document;
    document.add(std::make_unique<Field>(std::string(kIdField), FieldValue{std::to_string(id)}, id_type()));
    document.add(std::make_unique<Field>(std::string(kBodyField), FieldValue{body}, body_type()));
    return document;
}

int32_t id_of(int32_t thread, int32_t within) { return thread * kDocsPerThread + within; }

/// Runs `work(thread)` on `kThreads` threads and reports whether every call
/// succeeded.
template <typename Work>
bool run_on_threads(Work work) {
    std::atomic<int32_t> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int32_t thread = 0; thread < kThreads; ++thread) {
        threads.emplace_back([&failures, work, thread] {
            if (!work(thread)) {
                ++failures;
            }
        });
    }
    for (std::thread &thread : threads) {
        thread.join();
    }
    return failures.load() == 0;
}

void check_index(const std::filesystem::path &path) {
    auto directory = FSDirectory::open(path);
    ASSERT_TRUE(directory.has_value());
    CheckIndex check(*directory.value());
    auto checked = check.check();
    ASSERT_TRUE(checked.has_value()) << (checked.has_value() ? "" : checked.error().to_string());
    EXPECT_TRUE(checked.value().clean()) << checked.value().to_string();
}

} // namespace

TEST(IndexWriterConcurrencyTest, ConcurrentAddsUpdatesAndDeletesAreSerialized) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "clucene_writer_concurrency";
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);

    auto directory = FSDirectory::open(path);
    ASSERT_TRUE(directory.has_value());
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(*directory.value(), config);
    ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());

    // Every thread owns a disjoint range of ids, so the expected document count
    // is exact at every step even though the calls interleave.
    const bool added = run_on_threads([&writer](int32_t thread) {
        for (int32_t within = 0; within < kDocsPerThread; ++within) {
            if (!writer.value()->add_document(make_document(id_of(thread, within), std::string("add"))).has_value()) {
                return false;
            }
        }
        return true;
    });
    ASSERT_TRUE(added) << "a concurrent add failed";
    auto docs = writer.value()->num_docs();
    ASSERT_TRUE(docs.has_value()) << docs.error().to_string();
    EXPECT_EQ(docs.value(), kThreads * kDocsPerThread);

    const bool updated = run_on_threads([&writer](int32_t thread) {
        for (int32_t within = 0; within < kDocsPerThread; ++within) {
            const int32_t id = id_of(thread, within);
            auto term = Term::from_text(std::string(kIdField), std::to_string(id));
            if (!writer.value()->update_document(term, make_document(id, std::string("update"))).has_value()) {
                return false;
            }
        }
        return true;
    });
    ASSERT_TRUE(updated) << "a concurrent update failed";

    // An update buffers a delete that is applied when the readers are opened, so
    // the count is checked through a reader: every id must be there once, with the
    // body of its update.
    auto reader = writer.value()->get_reader();
    ASSERT_TRUE(reader.has_value()) << reader.error().to_string();
    IndexSearcher searcher(reader.value());
    auto total = searcher.count(MatchAllDocsQuery());
    ASSERT_TRUE(total.has_value()) << total.error().to_string();
    EXPECT_EQ(static_cast<int64_t>(total.value()), kThreads * kDocsPerThread);
    // Every document carries the body of its update, exactly once.
    auto updated_hits = searcher.count(TermQuery(Term::from_text(std::string(kBodyField), "update")));
    ASSERT_TRUE(updated_hits.has_value()) << updated_hits.error().to_string();
    EXPECT_EQ(static_cast<int64_t>(updated_hits.value()), kThreads * kDocsPerThread);
    auto add_hits = searcher.count(TermQuery(Term::from_text(std::string(kBodyField), "add")));
    ASSERT_TRUE(add_hits.has_value()) << add_hits.error().to_string();
    EXPECT_EQ(add_hits.value(), 0);

    const bool deleted = run_on_threads([&writer](int32_t thread) {
        for (int32_t within = 0; within < kDocsPerThread; ++within) {
            auto term = Term::from_text(std::string(kIdField), std::to_string(id_of(thread, within)));
            if (!writer.value()->delete_documents(term).has_value()) {
                return false;
            }
        }
        return true;
    });
    ASSERT_TRUE(deleted) << "a concurrent delete failed";

    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());

    auto reopened = DirectoryReader::open(*directory.value());
    ASSERT_TRUE(reopened.has_value()) << reopened.error().to_string();
    IndexSearcher reopened_searcher(reopened.value());
    auto remaining = reopened_searcher.count(MatchAllDocsQuery());
    ASSERT_TRUE(remaining.has_value()) << remaining.error().to_string();
    EXPECT_EQ(remaining.value(), 0);
    check_index(path);

    std::filesystem::remove_all(path, ignored);
}
