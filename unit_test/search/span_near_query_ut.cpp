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
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

module lucene_core:ut.span_near_query;

import lucene_core;

using namespace lucene;

namespace {

std::unique_ptr<Directory> build_near_index(const std::filesystem::path &path, const std::vector<std::string> &texts) {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);

    auto directory = FSDirectory::open(path);
    EXPECT_TRUE(directory.has_value());
    if (!directory.has_value()) {
        return nullptr;
    }

    FieldType body_type;
    EXPECT_TRUE(body_type.set_index_options(IndexOptions::DocsAndFreqsAndPositions).has_value());

    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(*directory.value(), config);
    EXPECT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());
    if (!writer.has_value()) {
        return nullptr;
    }
    for (const std::string &text : texts) {
        Document doc;
        doc.add(std::make_unique<Field>("body", FieldValue{text}, body_type));
        EXPECT_TRUE(writer.value()->add_document(std::move(doc)).has_value());
    }
    EXPECT_TRUE(writer.value()->commit().has_value());
    EXPECT_TRUE(writer.value()->close().has_value());
    return std::move(directory.value());
}

std::shared_ptr<SpanQuery> span_term(std::string_view field, std::string_view text) {
    return std::make_shared<SpanTermQuery>(Term::from_text(std::string(field), text));
}

int32_t count_near(IndexSearcher &searcher, std::vector<std::shared_ptr<SpanQuery>> clauses, int32_t slop,
                   bool in_order = true) {
    auto query = SpanNearQuery::create(std::move(clauses), slop, in_order);
    EXPECT_TRUE(query.has_value()) << (query.has_value() ? "" : query.error().to_string());
    if (!query.has_value()) {
        return -1;
    }
    auto count = searcher.count(query.value());
    EXPECT_TRUE(count.has_value()) << (count.has_value() ? "" : count.error().to_string());
    return count.value_or(-1);
}

} // namespace

TEST(SpanNearQueryTest, MatchesAdjacentTermsInOrder) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_span_near";
    auto directory = build_near_index(path, {"the quick brown fox", "quick brownie", "brown quick"});
    ASSERT_NE(directory, nullptr);

    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    IndexSearcher searcher(reader.value());

    // "the quick brown fox" has quick then brown adjacently.
    EXPECT_EQ(count_near(searcher, {span_term("body", "quick"), span_term("body", "brown")}, 0), 1);
    // Reverse order does not match with `in_order`.
    EXPECT_EQ(count_near(searcher, {span_term("body", "brown"), span_term("body", "quick")}, 0), 1);

    EXPECT_TRUE(reader.value()->close().has_value());
}

TEST(SpanNearQueryTest, CountsThePositionsBetweenTheSpans) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_span_near_slop";
    auto directory = build_near_index(path, {"quick brown fox", "quick brown fox"});
    ASSERT_NE(directory, nullptr);

    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    // `quick` at 0, `fox` at 2: one position in between.
    EXPECT_EQ(count_near(searcher, {span_term("body", "quick"), span_term("body", "fox")}, 0), 0);
    EXPECT_EQ(count_near(searcher, {span_term("body", "quick"), span_term("body", "fox")}, 1), 2);
    EXPECT_EQ(count_near(searcher, {span_term("body", "quick"), span_term("body", "fox")}, 2), 2);

    EXPECT_TRUE(reader.value()->close().has_value());
}

TEST(SpanNearQueryTest, RejectsInvalidClauses) {
    // Empty clause list.
    EXPECT_FALSE(SpanNearQuery::create({}, 0, true).has_value());
    // A negative slop is legal (it allows overlap), so only the operands are
    // validated.
    EXPECT_TRUE(SpanNearQuery::create({span_term("body", "a")}, -1, true).has_value());
    // Mixed fields.
    EXPECT_FALSE(SpanNearQuery::create({span_term("body", "a"), span_term("title", "b")}, 0, true).has_value());
}

TEST(SpanNearQueryTest, FormatsTheQuery) {
    auto query = SpanNearQuery::create({span_term("body", "quick"), span_term("body", "fox")}, 2, true);
    ASSERT_TRUE(query.has_value());
    auto text = query.value().to_string("body");
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text.value(), "spanNear([SpanTermQuery(quick), SpanTermQuery(fox)], 2, true)");

    auto same = SpanNearQuery::create({span_term("body", "quick"), span_term("body", "fox")}, 2, true);
    ASSERT_TRUE(same.has_value());
    EXPECT_TRUE(query.value().equals(same.value()));
    EXPECT_EQ(query.value().hash(), same.value().hash());

    auto other = SpanNearQuery::create({span_term("body", "quick"), span_term("body", "fox")}, 3, true);
    ASSERT_TRUE(other.has_value());
    EXPECT_FALSE(query.value().equals(other.value()));
}

TEST(SpanNearQueryTest, UnorderedAcceptsEitherOrder) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_span_near_unordered";
    auto directory = build_near_index(path, {"quick brown", "brown quick", "quick fox brown"});
    ASSERT_NE(directory, nullptr);

    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    // Ordered: only the document that really has `quick` before `brown`.
    EXPECT_EQ(count_near(searcher, {span_term("body", "quick"), span_term("body", "brown")}, 0, true), 1);

    // Unordered: both orders count, but the document with a word in between still
    // needs a slop of one.
    EXPECT_EQ(count_near(searcher, {span_term("body", "quick"), span_term("body", "brown")}, 0, false), 2);
    EXPECT_EQ(count_near(searcher, {span_term("body", "quick"), span_term("body", "brown")}, 1, false), 3);
    // The ordered query also accepts the far document once the slop allows it.
    EXPECT_EQ(count_near(searcher, {span_term("body", "quick"), span_term("body", "brown")}, 1, true), 2);

    EXPECT_TRUE(reader.value()->close().has_value());
}
