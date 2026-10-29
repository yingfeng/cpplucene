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

module lucene_core:ut.span_term_query;

import lucene_core;

using namespace lucene;

namespace {

/// Builds an index whose `body` field holds `texts` with positions, unless
/// `with_positions` is false.
std::unique_ptr<Directory> build_index(const std::filesystem::path &path, const std::vector<std::string> &texts,
                                       bool with_positions) {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);

    auto directory = FSDirectory::open(path);
    EXPECT_TRUE(directory.has_value()) << (directory.has_value() ? "" : directory.error().to_string());
    if (!directory.has_value()) {
        return nullptr;
    }

    FieldType body_type;
    EXPECT_TRUE(body_type
                        .set_index_options(with_positions ? IndexOptions::DocsAndFreqsAndPositions
                                                          : IndexOptions::DocsAndFreqs)
                        .has_value());

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

std::vector<int32_t> positions_of(SpanTermQuery &query, IndexSearcher &searcher, int32_t doc) {
    auto weight = query.create_weight(searcher, ScoreMode::Complete, 1.0F);
    EXPECT_TRUE(weight.has_value()) << (weight.has_value() ? "" : weight.error().to_string());
    if (!weight.has_value()) {
        return {};
    }
    auto *span_weight = dynamic_cast<SpanWeight *>(weight.value().get());
    EXPECT_NE(span_weight, nullptr);
    if (span_weight == nullptr) {
        return {};
    }

    auto leaves = searcher.reader().leaves();
    EXPECT_TRUE(leaves.has_value());
    if (!leaves.has_value()) {
        return {};
    }
    const LeafReaderContext &context = leaves.value().front();
    auto spans = span_weight->get_spans(context, SpanPostings::kPositions);
    EXPECT_TRUE(spans.has_value()) << (spans.has_value() ? "" : spans.error().to_string());
    if (!spans.has_value() || spans.value() == nullptr) {
        return {};
    }

    std::vector<int32_t> positions;
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
        positions.push_back(position.value());
    }
    return positions;
}

} // namespace

TEST(SpanTermQueryTest, CountsTheDocumentsThatHoldTheTerm) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_span_term";
    auto directory = build_index(path, {"the quick brown fox", "a lazy dog", "quick quick brown"}, true);
    ASSERT_NE(directory, nullptr);

    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    IndexSearcher searcher(reader.value());

    SpanTermQuery query(Term::from_text("body", "quick"));
    auto count = searcher.count(query);
    ASSERT_TRUE(count.has_value()) << (count.has_value() ? "" : count.error().to_string());
    EXPECT_EQ(count.value(), 2);

    auto top = searcher.search(query, 10);
    ASSERT_TRUE(top.has_value()) << (top.has_value() ? "" : top.error().to_string());
    EXPECT_EQ(top.value().total_hits.value, 2);

    EXPECT_TRUE(reader.value()->close().has_value());
}

TEST(SpanTermQueryTest, WalksThePositionsOfAMatch) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_span_positions";
    auto directory = build_index(path, {"the quick brown fox", "quick quick brown"}, true);
    ASSERT_NE(directory, nullptr);

    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    SpanTermQuery query(Term::from_text("body", "quick"));
    // `the quick brown fox`: `quick` sits at position 1.
    EXPECT_EQ(positions_of(query, searcher, 0), (std::vector<int32_t>{1}));
    // `quick quick brown`: positions 0 and 1.
    EXPECT_EQ(positions_of(query, searcher, 1), (std::vector<int32_t>{0, 1}));

    EXPECT_TRUE(reader.value()->close().has_value());
}

TEST(SpanTermQueryTest, RejectsAFieldIndexedWithoutPositions) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_span_no_positions";
    auto directory = build_index(path, {"the quick brown fox"}, false);
    ASSERT_NE(directory, nullptr);

    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    SpanTermQuery query(Term::from_text("body", "quick"));
    auto count = searcher.count(query);
    EXPECT_FALSE(count.has_value());
    if (!count.has_value()) {
        EXPECT_NE(count.error().to_string().find("without position data"), std::string::npos)
                << count.error().to_string();
    }

    EXPECT_TRUE(reader.value()->close().has_value());
}

TEST(SpanTermQueryTest, FormatsAndCompares) {
    SpanTermQuery query(Term::from_text("body", "quick"));
    auto text = query.to_string("body");
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text.value(), "SpanTermQuery(quick)");
    auto full = query.to_string("other");
    ASSERT_TRUE(full.has_value());
    EXPECT_EQ(full.value(), "SpanTermQuery(body:quick)");

    SpanTermQuery same(Term::from_text("body", "quick"));
    SpanTermQuery other(Term::from_text("body", "slow"));
    EXPECT_TRUE(query.equals(same));
    EXPECT_FALSE(query.equals(other));
    EXPECT_EQ(query.hash(), same.hash());
}
