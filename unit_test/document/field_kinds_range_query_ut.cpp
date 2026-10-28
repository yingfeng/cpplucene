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

// The range queries of the numeric field kinds: they answer an index-or-doc-values
// query, and the integer ones also try the index-sort shortcut, so both an index
// sorted by the field and a random one have to return the same documents.

module;

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.field_kinds_range_query;

import lucene_core;

using namespace lucene;

namespace {

constexpr int32_t kDocs = 40;

/// `num = doc`, `ival = doc`, `fval = doc / 2`, `dval = doc / 4`.
void build_index(Directory &directory, const std::optional<Sort> &index_sort) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_max_buffered_docs(9); // several segments
    if (index_sort.has_value()) {
        config.set_index_sort(index_sort.value());
    }
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());
    for (int32_t doc = 0; doc < kDocs; ++doc) {
        Document document;
        auto num = LongField::create("num", doc, false);
        ASSERT_TRUE(num.has_value()) << num.error().to_string();
        document.add(std::make_unique<Field>(std::move(num.value())));
        auto ival = IntField::create("ival", doc, false);
        ASSERT_TRUE(ival.has_value()) << ival.error().to_string();
        document.add(std::make_unique<Field>(std::move(ival.value())));
        auto fval = FloatField::create("fval", static_cast<float>(doc) / 2.0F, false);
        ASSERT_TRUE(fval.has_value()) << fval.error().to_string();
        document.add(std::make_unique<Field>(std::move(fval.value())));
        auto dval = DoubleField::create("dval", static_cast<double>(doc) / 4.0, false);
        ASSERT_TRUE(dval.has_value()) << dval.error().to_string();
        document.add(std::make_unique<Field>(std::move(dval.value())));
        ASSERT_TRUE(writer.value()->add_document(std::move(document)).has_value());
    }
    ASSERT_TRUE(writer.value()->close().has_value());
}

/// The documents of a query, sorted.
std::vector<int32_t> docs_of(IndexSearcher &searcher, const Query &query) {
    auto hits = searcher.search(query, kDocs);
    EXPECT_TRUE(hits.has_value()) << (hits.has_value() ? "" : hits.error().to_string());
    std::vector<int32_t> docs;
    if (!hits.has_value()) {
        return docs;
    }
    for (const ScoreDoc &hit : hits.value().score_docs) {
        docs.push_back(hit.doc);
    }
    std::sort(docs.begin(), docs.end());
    return docs;
}

std::vector<int32_t> expected(int32_t first, int32_t last) {
    std::vector<int32_t> docs;
    for (int32_t doc = first; doc <= last; ++doc) {
        docs.push_back(doc);
    }
    return docs;
}

/// Every field kind answers the same documents on a random and on an
/// index-sorted index.
void check_all_kinds(IndexSearcher &searcher) {
    auto long_range = LongField::new_range_query("num", 10, 19);
    ASSERT_TRUE(long_range.has_value()) << long_range.error().to_string();
    EXPECT_EQ(docs_of(searcher, *long_range.value()), expected(10, 19));
    auto counted = searcher.count(*long_range.value());
    ASSERT_TRUE(counted.has_value()) << counted.error().to_string();
    EXPECT_EQ(counted.value(), 10);

    auto long_exact = LongField::new_exact_query("num", 7);
    ASSERT_TRUE(long_exact.has_value()) << long_exact.error().to_string();
    EXPECT_EQ(docs_of(searcher, *long_exact.value()), expected(7, 7));

    auto int_range = IntField::new_range_query("ival", 30, 39);
    ASSERT_TRUE(int_range.has_value()) << int_range.error().to_string();
    EXPECT_EQ(docs_of(searcher, *int_range.value()), expected(30, 39));

    auto int_exact = IntField::new_exact_query("ival", 0);
    ASSERT_TRUE(int_exact.has_value()) << int_exact.error().to_string();
    EXPECT_EQ(docs_of(searcher, *int_exact.value()), expected(0, 0));

    // fval = doc / 2, so [4.0, 6.0] holds the documents 8..12.
    auto float_range = FloatField::new_range_query("fval", 4.0F, 6.0F);
    ASSERT_TRUE(float_range.has_value()) << float_range.error().to_string();
    EXPECT_EQ(docs_of(searcher, *float_range.value()), expected(8, 12));

    // dval = doc / 4, so [2.0, 3.0] holds the documents 8..12 as well.
    auto double_range = DoubleField::new_range_query("dval", 2.0, 3.0);
    ASSERT_TRUE(double_range.has_value()) << double_range.error().to_string();
    EXPECT_EQ(docs_of(searcher, *double_range.value()), expected(8, 12));

    // A range without any value matches nothing.
    auto empty = LongField::new_range_query("num", 100, 200);
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(docs_of(searcher, *empty.value()).empty());
}

} // namespace

TEST(FieldKindsRangeQueryTest, TheRangesMatchOnARandomIndex) {
    ByteBuffersDirectory directory;
    build_index(directory, std::nullopt);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    ASSERT_GT(reader.value()->leaves().value().size(), 1U);
    IndexSearcher searcher(reader.value());
    check_all_kinds(searcher);
}

TEST(FieldKindsRangeQueryTest, TheRangesMatchOnAnIndexSortedByTheField) {
    auto index_sort = Sort::with_fields({SortField::with_reverse("num", SortFieldType::kLong, false).value()});
    ASSERT_TRUE(index_sort.has_value());

    ByteBuffersDirectory directory;
    build_index(directory, index_sort.value());
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    ASSERT_GT(reader.value()->leaves().value().size(), 1U);
    IndexSearcher searcher(reader.value());
    check_all_kinds(searcher);

    // The index sort really is the field the shortcut looks at.
    for (const LeafReaderContext &leaf : reader.value()->leaves().value()) {
        auto metadata = leaf.reader()->leaf_meta_data();
        ASSERT_NE(metadata.sort(), nullptr);
        ASSERT_FALSE(metadata.sort()->fields().empty());
        EXPECT_EQ(metadata.sort()->fields().front().field(), "num");
    }
}
