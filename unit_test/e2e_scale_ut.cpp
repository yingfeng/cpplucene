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

// Scale test: build a large index, query it, merge it and check it, printing how
// long each step took.  It is opt-in because it takes seconds and hundreds of
// megabytes:
//
//   CLUCENE_SCALE_TEST=1 ./build/unit_test/unit_test --gtest_filter="EndToEndScaleTest.*"
//   CLUCENE_SCALE_TEST=1 CLUCENE_SCALE_DOCS=1000000 ./build/unit_test/unit_test --gtest_filter=...
//
// `CLUCENE_SCALE_DOCS` defaults to 100000.

module;

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.e2e_scale;

import lucene_core;

using namespace lucene;

namespace {

/// Documents the run builds; the environment can change it.
int32_t scale_docs() {
    const char *configured = std::getenv("CLUCENE_SCALE_DOCS");
    if (configured == nullptr) {
        return 100000;
    }
    const int value = std::atoi(configured);
    return value > 0 ? value : 100000;
}

/// `true` when the scale test was asked for.
bool is_enabled() { return std::getenv("CLUCENE_SCALE_TEST") != nullptr; }

using Clock = std::chrono::steady_clock;

double millis_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    return type;
}

FieldType id_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::Docs).has_value());
    return type;
}

/// A document with the same shape as the small e2e one, without the stored text.
Document scale_doc(int32_t id) {
    Document doc;
    doc.add(std::make_unique<Field>("id", FieldValue{std::to_string(id)}, id_type()));
    doc.add(std::make_unique<Field>("body", FieldValue{std::string("token") + std::to_string(id % 1000)},
                                    text_type()));

    auto price = NumericDocValuesField::create("price", id);
    EXPECT_TRUE(price.has_value());
    doc.add(std::make_unique<Field>(std::move(price.value())));

    // Indexed for the term range query and stored as doc values for sorting.
    doc.add(std::make_unique<Field>("brand", FieldValue{std::string("brand") + std::to_string(id % 10)},
                                    id_type()));
    auto brand = SortedDocValuesField::create("brand", std::string("brand") + std::to_string(id % 10));
    EXPECT_TRUE(brand.has_value());
    doc.add(std::make_unique<Field>(std::move(brand.value())));

    const int32_t point[1] = {id % 1000};
    auto bucket = IntPoint::create("bucket", std::span<const int32_t>(point, 1));
    EXPECT_TRUE(bucket.has_value());
    doc.add(std::make_unique<Field>(std::move(bucket.value())));
    return doc;
}

} // namespace

TEST(EndToEndScaleTest, BuildsQueriesMergesAndChecksALargeIndex) {
    if (!is_enabled()) {
        GTEST_SKIP() << "set CLUCENE_SCALE_TEST=1 to run the scale test";
    }
    const int32_t docs = scale_docs();
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_e2e_scale";
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
    std::printf("[scale] %d documents\n", docs);

    // ---- build ----
    auto build_start = Clock::now();
    {
        auto directory = FSDirectory::open(path);
        ASSERT_TRUE(directory.has_value()) << (directory ? "" : directory.error().to_string());
        WhitespaceAnalyzer analyzer;
        IndexWriterConfig config(analyzer);
        config.set_max_buffered_docs(20000);
        auto writer = IndexWriter::open(*directory.value(), config);
        ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

        for (int32_t i = 0; i < docs; ++i) {
            ASSERT_TRUE(writer.value()->add_document(scale_doc(i)).has_value());
        }
        const auto add_ms = millis_since(build_start);
        auto commit_start = Clock::now();
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
        const auto commit_ms = millis_since(commit_start);
        std::printf("[scale] added %d documents in %.0f ms, commit in %.0f ms, %zu segments\n", docs, add_ms,
                    commit_ms, writer.value()->segment_count());

        auto merge_start = Clock::now();
        ASSERT_TRUE(writer.value()->force_merge(1).has_value());
        std::printf("[scale] force_merge(1) in %.0f ms, %zu segments\n", millis_since(merge_start),
                    writer.value()->segment_count());
        ASSERT_TRUE(writer.value()->close().has_value());
        ASSERT_TRUE(directory.value()->sync_metadata().has_value());
    }
    std::printf("[scale] build total %.0f ms\n", millis_since(build_start));

    // ---- open and query ----
    auto directory = FSDirectory::open(path);
    ASSERT_TRUE(directory.has_value());
    auto open_start = Clock::now();
    auto reader = DirectoryReader::open(*directory.value());
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    std::printf("[scale] open in %.0f ms, %d documents in %zu leaves\n", millis_since(open_start),
                reader.value()->num_docs().value(), reader.value()->leaves().value().size());
    EXPECT_EQ(reader.value()->num_docs().value(), docs);
    IndexSearcher searcher(reader.value());

    auto timed_count = [&searcher](const char *label, const Query &query) {
        auto start = Clock::now();
        auto count = searcher.count(query);
        EXPECT_TRUE(count.has_value()) << (count ? "" : count.error().to_string());
        std::printf("[scale] count %-18s %8d hits in %8.1f ms\n", label, count.value_or(-1),
                    millis_since(start));
        return count.value_or(-1);
    };

    EXPECT_EQ(timed_count("match_all", MatchAllDocsQuery()), docs);
    EXPECT_EQ(timed_count("term", TermQuery(Term::from_text("body", "token7"))), docs / 1000);

    auto range = IntPoint::new_range_query("bucket", 0, 9);
    ASSERT_TRUE(range.has_value());
    EXPECT_EQ(timed_count("point_range", *range.value()), docs / 100);

    auto brands = TermRangeQuery::create_string_range("brand", "brand1", "brand3", true, true);
    ASSERT_TRUE(brands.has_value());
    EXPECT_EQ(timed_count("term_range", brands.value()), docs * 3 / 10);

    // ---- top hits with a sort on doc values ----
    auto by_price = SortField::with_reverse("price", SortFieldType::kLong, true);
    ASSERT_TRUE(by_price.has_value());
    auto sort = Sort::with_fields({by_price.value()});
    auto sort_start = Clock::now();
    auto sorted = searcher.search(MatchAllDocsQuery(), sort.value(), 20);
    ASSERT_TRUE(sorted.has_value()) << (sorted ? "" : sorted.error().to_string());
    std::printf("[scale] sorted top 20 in %.1f ms\n", millis_since(sort_start));
    ASSERT_EQ(sorted.value().score_docs.size(), 20U);
    // Descending by price: the first hit is the largest document id.
    EXPECT_EQ(std::get<int64_t>(sorted.value().score_docs.front().fields.front()), docs - 1);

    auto relevance_start = Clock::now();
    auto relevance = searcher.search(TermQuery(Term::from_text("body", "token7")), 20);
    ASSERT_TRUE(relevance.has_value()) << (relevance ? "" : relevance.error().to_string());
    std::printf("[scale] relevance top 20 in %.1f ms\n", millis_since(relevance_start));
    EXPECT_EQ(relevance.value().total_hits.value, docs / 1000);

    // ---- check the whole index ----
    auto check_start = Clock::now();
    CheckIndex check(*directory.value());
    auto checked = check.check();
    ASSERT_TRUE(checked.has_value()) << (checked ? "" : checked.error().to_string());
    std::printf("[scale] CheckIndex in %.0f ms\n", millis_since(check_start));
    EXPECT_TRUE(checked.value().clean()) << checked.value().to_string();
    EXPECT_EQ(checked.value().total_docs(), docs);

    std::filesystem::remove_all(path, ignored);
}
