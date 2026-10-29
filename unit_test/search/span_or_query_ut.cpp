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

module lucene_core:ut.span_or_query;

import lucene_core;

using namespace lucene;

namespace {

std::unique_ptr<Directory> build_or_index(const std::filesystem::path &path, const std::vector<std::string> &texts) {
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

std::shared_ptr<SpanQuery> or_term(std::string_view field, std::string_view text) {
    return std::make_shared<SpanTermQuery>(Term::from_text(std::string(field), text));
}

/// The `(start, end)` pairs the spans of `doc` produce.
std::vector<std::pair<int32_t, int32_t>> span_positions(SpanQuery &query, IndexSearcher &searcher, int32_t doc) {
    std::vector<std::pair<int32_t, int32_t>> positions;
    auto weight = query.create_weight(searcher, ScoreMode::Complete, 1.0F);
    EXPECT_TRUE(weight.has_value()) << (weight.has_value() ? "" : weight.error().to_string());
    if (!weight.has_value()) {
        return positions;
    }
    auto *span_weight = dynamic_cast<SpanWeight *>(weight.value().get());
    EXPECT_NE(span_weight, nullptr);
    if (span_weight == nullptr) {
        return positions;
    }
    auto leaves = searcher.reader().leaves();
    EXPECT_TRUE(leaves.has_value());
    if (!leaves.has_value()) {
        return positions;
    }
    auto spans = span_weight->get_spans(leaves.value().front(), SpanPostings::kPositions);
    EXPECT_TRUE(spans.has_value()) << (spans.has_value() ? "" : spans.error().to_string());
    if (!spans.has_value() || spans.value() == nullptr) {
        return positions;
    }
    auto advanced = spans.value()->advance(doc);
    EXPECT_TRUE(advanced.has_value());
    if (!advanced.has_value() || advanced.value() != doc) {
        return positions;
    }
    while (true) {
        auto position = spans.value()->next_start_position();
        EXPECT_TRUE(position.has_value());
        if (!position.has_value() || position.value() == NO_MORE_POSITIONS) {
            break;
        }
        positions.emplace_back(spans.value()->start_position(), spans.value()->end_position());
    }
    return positions;
}

} // namespace

TEST(SpanOrQueryTest, MatchesTheUnionOfItsClauses) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_span_or";
    auto directory = build_or_index(path, {"quick brown fox", "lazy dog", "lazy fox"});
    ASSERT_NE(directory, nullptr);

    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    IndexSearcher searcher(reader.value());

    auto query = SpanOrQuery::create({or_term("body", "quick"), or_term("body", "dog")});
    ASSERT_TRUE(query.has_value()) << (query.has_value() ? "" : query.error().to_string());
    auto count = searcher.count(query.value());
    ASSERT_TRUE(count.has_value()) << (count.has_value() ? "" : count.error().to_string());
    // `quick` is in doc 0, `dog` in doc 1.
    EXPECT_EQ(count.value(), 2);

    // Every document that holds either term counts once, even when both clauses
    // match it: doc 0 through `fox`, doc 1 through `lazy`, doc 2 through both.
    auto overlapping = SpanOrQuery::create({or_term("body", "fox"), or_term("body", "lazy")});
    ASSERT_TRUE(overlapping.has_value());
    auto overlapping_count = searcher.count(overlapping.value());
    ASSERT_TRUE(overlapping_count.has_value());
    EXPECT_EQ(overlapping_count.value(), 3);

    EXPECT_TRUE(reader.value()->close().has_value());
}

TEST(SpanOrQueryTest, MergesThePositionsOfTheClauses) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_span_or_positions";
    auto directory = build_or_index(path, {"quick brown fox"});
    ASSERT_NE(directory, nullptr);

    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    auto query = SpanOrQuery::create({or_term("body", "fox"), or_term("body", "quick")});
    ASSERT_TRUE(query.has_value());

    // `quick` at 0 and `fox` at 2, in increasing start order regardless of the
    // clause order.
    EXPECT_EQ(span_positions(query.value(), searcher, 0), (std::vector<std::pair<int32_t, int32_t>>{{0, 1}, {2, 3}}));

    // Repeated positions are reported once per clause that holds them: two
    // clauses for the same term both match.
    auto duplicate = SpanOrQuery::create({or_term("body", "fox"), or_term("body", "fox")});
    ASSERT_TRUE(duplicate.has_value());
    EXPECT_EQ(span_positions(duplicate.value(), searcher, 0),
              (std::vector<std::pair<int32_t, int32_t>>{{2, 3}, {2, 3}}));

    EXPECT_TRUE(reader.value()->close().has_value());
}

TEST(SpanOrQueryTest, CombinesWithNearAndTermClauses) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_span_or_nested";
    auto directory = build_or_index(path, {"quick brown fox", "brown quick"});
    ASSERT_NE(directory, nullptr);

    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    auto near = SpanNearQuery::create({or_term("body", "quick"), or_term("body", "brown")}, 0, true);
    ASSERT_TRUE(near.has_value());
    auto nested = SpanOrQuery::create({std::make_shared<SpanNearQuery>(std::move(near.value())),
                                       or_term("body", "fox")});
    ASSERT_TRUE(nested.has_value());
    auto count = searcher.count(nested.value());
    ASSERT_TRUE(count.has_value()) << (count.has_value() ? "" : count.error().to_string());
    // Doc 0 matches the near clause (`quick` then `brown`) and the term;
    // doc 1 has `brown quick`, which the ordered near clause rejects.
    EXPECT_EQ(count.value(), 1);

    EXPECT_TRUE(reader.value()->close().has_value());
}

TEST(SpanOrQueryTest, RejectsInvalidClauses) {
    EXPECT_FALSE(SpanOrQuery::create({}).has_value());
    EXPECT_FALSE(SpanOrQuery::create({or_term("body", "a"), or_term("title", "b")}).has_value());
}

TEST(SpanOrQueryTest, FormatsAndCompares) {
    auto query = SpanOrQuery::create({or_term("body", "quick"), or_term("body", "fox")});
    ASSERT_TRUE(query.has_value());
    auto text = query.value().to_string("body");
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text.value(), "spanOr([SpanTermQuery(quick), SpanTermQuery(fox)])");

    auto same = SpanOrQuery::create({or_term("body", "quick"), or_term("body", "fox")});
    ASSERT_TRUE(same.has_value());
    EXPECT_TRUE(query.value().equals(same.value()));
    EXPECT_EQ(query.value().hash(), same.value().hash());

    auto reordered = SpanOrQuery::create({or_term("body", "fox"), or_term("body", "quick")});
    ASSERT_TRUE(reordered.has_value());
    EXPECT_FALSE(query.value().equals(reordered.value()));
}
