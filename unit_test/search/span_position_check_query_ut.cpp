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

module lucene_core:ut.span_position_check_query;

import lucene_core;

using namespace lucene;

namespace {

std::unique_ptr<Directory> build_check_index(const std::filesystem::path &path,
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

std::shared_ptr<SpanQuery> check_term(std::string_view field, std::string_view text) {
    return std::make_shared<SpanTermQuery>(Term::from_text(std::string(field), text));
}

int32_t count_query(IndexSearcher &searcher, const Query &query) {
    auto count = searcher.count(query);
    EXPECT_TRUE(count.has_value()) << (count.has_value() ? "" : count.error().to_string());
    return count.value_or(-1);
}

} // namespace

TEST(SpanFirstQueryTest, KeepsOnlySpansAtTheStartOfTheField) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_span_first";
    auto directory = build_check_index(path, {"the quick brown fox", "quick brown"});
    ASSERT_NE(directory, nullptr);

    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    IndexSearcher searcher(reader.value());

    // `quick` sits at position 1 in doc 0 (after `the`) and at 0 in doc 1, so only
    // the second document starts with it within one position.
    SpanFirstQuery first_quick(check_term("body", "quick"), 1);
    EXPECT_EQ(count_query(searcher, first_quick), 1);

    // Widening the window to two positions accepts both documents.
    SpanFirstQuery wide(check_term("body", "quick"), 2);
    EXPECT_EQ(count_query(searcher, wide), 2);

    // `the` starts doc 0 and does not occur in doc 1.
    SpanFirstQuery first_the(check_term("body", "the"), 1);
    EXPECT_EQ(count_query(searcher, first_the), 1);

    EXPECT_TRUE(reader.value()->close().has_value());
}

TEST(SpanPositionRangeQueryTest, KeepsSpansInsideTheRange) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_span_pos_range";
    auto directory = build_check_index(path, {"the quick brown fox", "quick brown"});
    ASSERT_NE(directory, nullptr);

    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    // `quick` at 1 only: doc 0 qualifies, doc 1 starts at 0.
    SpanPositionRangeQuery second_position(check_term("body", "quick"), 1, 3);
    EXPECT_EQ(count_query(searcher, second_position), 1);

    // The whole range accepts both.
    SpanPositionRangeQuery everywhere(check_term("body", "quick"), 0, 10);
    EXPECT_EQ(count_query(searcher, everywhere), 2);

    // An empty range accepts nothing.
    SpanPositionRangeQuery empty(check_term("body", "quick"), 5, 5);
    EXPECT_EQ(count_query(searcher, empty), 0);

    EXPECT_TRUE(reader.value()->close().has_value());
}

TEST(SpanPositionCheckQueryTest, FiltersTheWrappedSpansPositions) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_span_check_positions";
    auto directory = build_check_index(path, {"quick brown quick"});
    ASSERT_NE(directory, nullptr);

    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    // Only the second `quick`, at position 2, is inside the range.
    SpanPositionRangeQuery query(check_term("body", "quick"), 2, 3);
    auto weight = query.create_weight(searcher, ScoreMode::Complete, 1.0F);
    ASSERT_TRUE(weight.has_value()) << (weight.has_value() ? "" : weight.error().to_string());
    auto *span_weight = dynamic_cast<SpanWeight *>(weight.value().get());
    ASSERT_NE(span_weight, nullptr);

    auto leaves = searcher.reader().leaves();
    ASSERT_TRUE(leaves.has_value());
    auto spans = span_weight->get_spans(leaves.value().front(), SpanPostings::kPositions);
    ASSERT_TRUE(spans.has_value()) << (spans.has_value() ? "" : spans.error().to_string());
    ASSERT_NE(spans.value(), nullptr);

    auto advanced = spans.value()->advance(0);
    ASSERT_TRUE(advanced.has_value());
    ASSERT_EQ(advanced.value(), 0);

    std::vector<int32_t> positions;
    while (true) {
        auto position = spans.value()->next_start_position();
        ASSERT_TRUE(position.has_value());
        if (position.value() == NO_MORE_POSITIONS) {
            break;
        }
        positions.push_back(position.value());
    }
    EXPECT_EQ(positions, (std::vector<int32_t>{2}));

    EXPECT_TRUE(reader.value()->close().has_value());
}

TEST(SpanPositionCheckQueryTest, FormatsAndCompares) {
    SpanFirstQuery first(check_term("body", "quick"), 2);
    auto first_text = first.to_string("body");
    ASSERT_TRUE(first_text.has_value());
    EXPECT_EQ(first_text.value(), "spanFirst(SpanTermQuery(quick), 2)");

    SpanPositionRangeQuery range(check_term("body", "quick"), 1, 3);
    auto range_text = range.to_string("body");
    ASSERT_TRUE(range_text.has_value());
    EXPECT_EQ(range_text.value(), "spanPosRange(SpanTermQuery(quick), 1, 3)");

    SpanPositionRangeQuery same(check_term("body", "quick"), 1, 3);
    EXPECT_TRUE(range.equals(same));
    EXPECT_EQ(range.hash(), same.hash());

    SpanPositionRangeQuery different(check_term("body", "quick"), 1, 4);
    EXPECT_FALSE(range.equals(different));

    // A first query is never equal to a range query, even with the same bounds.
    SpanPositionRangeQuery zero_based(check_term("body", "quick"), 0, 2);
    EXPECT_FALSE(first.equals(zero_based));
}
