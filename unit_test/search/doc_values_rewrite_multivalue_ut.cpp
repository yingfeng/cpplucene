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

// A multi-term query rewritten onto doc values, on a field with one value per
// document (the singleton fast path) and on a field with several (the sorted set
// path): both have to find the same documents a scan finds.

module;

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.doc_values_rewrite_multivalue;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kMultiField = "tags";
constexpr std::string_view kSingleField = "single";
constexpr int32_t kDocs = 6;

/// One value per document for the single-valued field, and zero to two for the
/// multi-valued one.
const std::vector<std::vector<std::string>> &multi_values() {
    static const std::vector<std::vector<std::string>> values{
            {"a"}, {"b"}, {"a", "b"}, {"c"}, {}, {"a", "c"}};
    return values;
}

const std::vector<std::string> &single_values() {
    static const std::vector<std::string> values{"aa", "bb", "aa", "cc", "bb", "dd"};
    return values;
}

void build(Directory &directory) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());
    for (int32_t doc = 0; doc < kDocs; ++doc) {
        Document document;
        for (const std::string &value : multi_values()[static_cast<size_t>(doc)]) {
            auto field = SortedSetDocValuesField::create(std::string(kMultiField), value);
            ASSERT_TRUE(field.has_value()) << field.error().to_string();
            document.add(std::make_unique<Field>(std::move(field.value())));
        }
        // A `Sorted` field holds one value per document, which is what the
        // singleton fast path of the doc values rewrite is for.
        auto single = SortedDocValuesField::create(std::string(kSingleField), single_values()[static_cast<size_t>(doc)]);
        ASSERT_TRUE(single.has_value()) << single.error().to_string();
        document.add(std::make_unique<Field>(std::move(single.value())));
        ASSERT_TRUE(writer.value()->add_document(std::move(document)).has_value());
    }
    ASSERT_TRUE(writer.value()->close().has_value());
}

/// Runs the range `[lower, upper]` with the doc values rewrite method; the rewrite
/// keeps the query alive, so it has to be owned by a shared pointer.
std::vector<int32_t> search_range(IndexSearcher &searcher, const std::string &field, const std::string &lower,
                                  const std::string &upper) {
    auto built = TermRangeQuery::with_string_range_rewrite(field, lower, upper, true, true,
                                                          MultiTermRewriteMethod::kDocValues);
    EXPECT_TRUE(built.has_value()) << (built.has_value() ? "" : built.error().to_string());
    std::vector<int32_t> docs;
    if (!built.has_value()) {
        return docs;
    }
    const auto query = std::make_shared<TermRangeQuery>(std::move(built.value()));
    auto hits = searcher.search(*query, kDocs);
    EXPECT_TRUE(hits.has_value()) << (hits.has_value() ? "" : hits.error().to_string());
    if (!hits.has_value()) {
        return docs;
    }
    for (const ScoreDoc &hit : hits.value().score_docs) {
        docs.push_back(hit.doc);
    }
    std::sort(docs.begin(), docs.end());
    return docs;
}

} // namespace

TEST(DocValuesRewriteMultiValueTest, AFieldWithSeveralValuesPerDocumentMatchesTheScan) {
    ByteBuffersDirectory directory;
    build(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    IndexSearcher searcher(reader.value());

    // Documents holding "a" or "b": 0, 1, 2 and 5.
    EXPECT_EQ(search_range(searcher, std::string(kMultiField), "a", "b"), (std::vector<int32_t>{0, 1, 2, 5}));
    // Documents holding "b" or "c": 1, 2, 3 and 5.
    EXPECT_EQ(search_range(searcher, std::string(kMultiField), "b", "c"), (std::vector<int32_t>{1, 2, 3, 5}));
    // A range without any value matches nothing.
    EXPECT_TRUE(search_range(searcher, std::string(kMultiField), "x", "z").empty());
}

TEST(DocValuesRewriteMultiValueTest, ASingleValuedFieldTakesTheFastPath) {
    ByteBuffersDirectory directory;
    build(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    // A `Sorted` field is handed out as a sorted set of one value per document, and
    // the multi-valued field keeps its own cursor: that is what the rewrite picks
    // its iterator from.
    LeafReader *leaf = reader.value()->leaves().value().front().reader();
    auto single = leaf->get_sorted_set_doc_values(std::string(kSingleField));
    ASSERT_TRUE(single.has_value()) << single.error().to_string();
    ASSERT_NE(single.value(), nullptr);
    EXPECT_NE(dynamic_cast<SingletonSortedSetDocValues *>(single.value().get()), nullptr);
    auto multi = leaf->get_sorted_set_doc_values(std::string(kMultiField));
    ASSERT_TRUE(multi.has_value()) << multi.error().to_string();
    ASSERT_NE(multi.value(), nullptr);
    EXPECT_EQ(dynamic_cast<SingletonSortedSetDocValues *>(multi.value().get()), nullptr);

    // 0 aa, 1 bb, 2 aa, 3 cc, 4 bb, 5 dd.
    EXPECT_EQ(search_range(searcher, std::string(kSingleField), "aa", "bb"), (std::vector<int32_t>{0, 1, 2, 4}));
    EXPECT_EQ(search_range(searcher, std::string(kSingleField), "cc", "dd"), (std::vector<int32_t>{3, 5}));
    EXPECT_TRUE(search_range(searcher, std::string(kSingleField), "a", "a").empty());
}
