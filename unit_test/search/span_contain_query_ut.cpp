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

module lucene_core:ut.span_contain_query;

import lucene_core;

using namespace lucene;

namespace {

std::unique_ptr<Directory> build_contain_index(const std::filesystem::path &path,
                                              const std::vector<std::string> &texts) {
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

std::shared_ptr<SpanQuery> contain_term(std::string_view field, std::string_view text) {
    return std::make_shared<SpanTermQuery>(Term::from_text(std::string(field), text));
}

std::shared_ptr<SpanQuery> phrase_of(std::string_view field, const std::vector<std::string> &words) {
    std::vector<std::shared_ptr<SpanQuery>> clauses;
    clauses.reserve(words.size());
    for (const std::string &word : words) {
        clauses.push_back(contain_term(field, word));
    }
    auto near = SpanNearQuery::create(std::move(clauses), 0, true);
    EXPECT_TRUE(near.has_value()) << (near.has_value() ? "" : near.error().to_string());
    return std::make_shared<SpanNearQuery>(std::move(near.value()));
}

/// The `[start, end)` of the first reported span of `doc`.
std::vector<std::pair<int32_t, int32_t>> contain_positions(SpanQuery &query, IndexSearcher &searcher, int32_t doc) {
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

TEST(SpanContainingQueryTest, ReportsTheBigSpan) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_span_containing";
    auto directory = build_contain_index(path, {"quick brown fox", "lazy dog"});
    ASSERT_NE(directory, nullptr);

    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    IndexSearcher searcher(reader.value());

    auto query = SpanContainingQuery::create(phrase_of("body", {"quick", "brown"}), contain_term("body", "brown"));
    ASSERT_TRUE(query.has_value()) << (query.has_value() ? "" : query.error().to_string());

    auto count = searcher.count(query.value());
    ASSERT_TRUE(count.has_value()) << (count.has_value() ? "" : count.error().to_string());
    EXPECT_EQ(count.value(), 1);

    // The reported span is the `big` one: `quick brown` at [0, 2).
    EXPECT_EQ(contain_positions(query.value(), searcher, 0),
              (std::vector<std::pair<int32_t, int32_t>>{{0, 2}}));

    EXPECT_TRUE(reader.value()->close().has_value());
}

TEST(SpanContainingQueryTest, RejectsPairsThatDoNotNest) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_span_containing_negative";
    auto directory = build_contain_index(path, {"quick brown fox"});
    ASSERT_NE(directory, nullptr);

    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    // `quick` ends before `brown` starts, so neither contains the other.
    auto query = SpanContainingQuery::create(contain_term("body", "brown"), contain_term("body", "quick"));
    ASSERT_TRUE(query.has_value());
    auto count = searcher.count(query.value());
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(count.value(), 0);

    EXPECT_TRUE(reader.value()->close().has_value());
}

TEST(SpanWithinQueryTest, ReportsTheLittleSpan) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_span_within";
    auto directory = build_contain_index(path, {"quick brown fox", "lazy dog"});
    ASSERT_NE(directory, nullptr);

    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    auto query = SpanWithinQuery::create(phrase_of("body", {"quick", "brown"}), contain_term("body", "brown"));
    ASSERT_TRUE(query.has_value()) << (query.has_value() ? "" : query.error().to_string());

    auto count = searcher.count(query.value());
    ASSERT_TRUE(count.has_value()) << (count.has_value() ? "" : count.error().to_string());
    EXPECT_EQ(count.value(), 1);

    // The reported span is the `little` one: `brown` at [1, 2).
    EXPECT_EQ(contain_positions(query.value(), searcher, 0),
              (std::vector<std::pair<int32_t, int32_t>>{{1, 2}}));

    EXPECT_TRUE(reader.value()->close().has_value());
}

TEST(SpanWithinQueryTest, RejectsAPhraseThatIsNotInsideASingleTerm) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_span_within_negative";
    auto directory = build_contain_index(path, {"quick brown fox"});
    ASSERT_NE(directory, nullptr);

    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    // A single term cannot contain the two-word phrase.
    auto query = SpanWithinQuery::create(contain_term("body", "brown"), phrase_of("body", {"quick", "brown"}));
    ASSERT_TRUE(query.has_value());
    auto count = searcher.count(query.value());
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(count.value(), 0);

    EXPECT_TRUE(reader.value()->close().has_value());
}

TEST(SpanContainQueryTest, RejectsInvalidOperandsAndFormats) {
    EXPECT_FALSE(SpanContainingQuery::create(nullptr, contain_term("body", "a")).has_value());
    EXPECT_FALSE(SpanWithinQuery::create(contain_term("body", "a"), contain_term("title", "b")).has_value());

    auto containing = SpanContainingQuery::create(contain_term("body", "a"), contain_term("body", "b"));
    ASSERT_TRUE(containing.has_value());
    auto containing_text = containing.value().to_string("body");
    ASSERT_TRUE(containing_text.has_value());
    EXPECT_EQ(containing_text.value(), "SpanContaining(SpanTermQuery(a), SpanTermQuery(b))");

    auto within = SpanWithinQuery::create(contain_term("body", "a"), contain_term("body", "b"));
    ASSERT_TRUE(within.has_value());
    auto within_text = within.value().to_string("body");
    ASSERT_TRUE(within_text.has_value());
    EXPECT_EQ(within_text.value(), "SpanWithin(SpanTermQuery(a), SpanTermQuery(b))");

    // The two subclasses never compare equal, even with identical operands.
    EXPECT_FALSE(containing.value().equals(within.value()));

    auto same = SpanContainingQuery::create(contain_term("body", "a"), contain_term("body", "b"));
    ASSERT_TRUE(same.has_value());
    EXPECT_TRUE(containing.value().equals(same.value()));
    EXPECT_EQ(containing.value().hash(), same.value().hash());
}
