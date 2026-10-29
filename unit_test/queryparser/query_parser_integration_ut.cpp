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

module lucene_core:ut.query_parser_integration;

import lucene_core;

using namespace lucene;

namespace {

std::int32_t count_hits(QueryParser &parser, IndexSearcher &searcher, std::string_view text) {
    auto query = parser.parse(text);
    EXPECT_TRUE(query.has_value()) << (query.has_value() ? std::string() : query.error().to_string());
    if (!query.has_value() || query.value() == nullptr) {
        return -1;
    }
    auto hits = searcher.count(*query.value());
    EXPECT_TRUE(hits.has_value()) << (hits.has_value() ? std::string() : hits.error().to_string());
    return hits.value_or(-1);
}

} // namespace

TEST(QueryParserIntegrationTest, ParsesQueriesAgainstAnEnglishIndex) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_query_parser_integration";
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);

    FieldType body_type;
    ASSERT_TRUE(body_type.set_index_options(IndexOptions::DocsAndFreqsAndPositions).has_value());

    {
        auto directory = FSDirectory::open(path);
        ASSERT_TRUE(directory.has_value()) << (directory.has_value() ? "" : directory.error().to_string());

        EnglishAnalyzer analyzer;
        IndexWriterConfig config(analyzer);
        auto writer = IndexWriter::open(*directory.value(), config);
        ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());

        const std::vector<std::string> texts{"The quick brown fox", "A lazy dog sleeps", "Foxes are running quickly"};
        for (const std::string &text : texts) {
            Document doc;
            doc.add(std::make_unique<Field>("body", FieldValue{text}, body_type));
            ASSERT_TRUE(writer.value()->add_document(std::move(doc)).has_value());
        }
        ASSERT_TRUE(writer.value()->commit().has_value());
        ASSERT_TRUE(writer.value()->close().has_value());
    }

    auto directory = FSDirectory::open(path);
    ASSERT_TRUE(directory.has_value());
    auto reader = DirectoryReader::open(*directory.value());
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    IndexSearcher searcher(reader.value());

    EnglishAnalyzer analyzer;
    QueryParser parser("body", analyzer);

    // `foxes` stems to `fox`, so the term matches two documents.
    EXPECT_EQ(count_hits(parser, searcher, "fox"), 2);
    // The chain lower cases, so an upper case query sees the same index.
    EXPECT_EQ(count_hits(parser, searcher, "FOX"), 2);
    EXPECT_EQ(count_hits(parser, searcher, "running"), 1);
    // A phrase needs the two terms at consecutive positions.
    EXPECT_EQ(count_hits(parser, searcher, "\"quick brown\""), 1);
    // `AND` requires both, `OR` either, `NOT` excludes.
    EXPECT_EQ(count_hits(parser, searcher, "fox AND lazy"), 0);
    EXPECT_EQ(count_hits(parser, searcher, "fox OR lazy"), 3);
    EXPECT_EQ(count_hits(parser, searcher, "fox NOT quick"), 1);
    // A prefix query does not go through the analysis chain.
    EXPECT_EQ(count_hits(parser, searcher, "fox*"), 2);

    ASSERT_TRUE(reader.value()->close().has_value());
}

TEST(QueryParserIntegrationTest, ParsesChineseQueriesAgainstABigramIndex) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_query_parser_cjk";
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);

    FieldType body_type;
    ASSERT_TRUE(body_type.set_index_options(IndexOptions::DocsAndFreqs).has_value());

    {
        auto directory = FSDirectory::open(path);
        ASSERT_TRUE(directory.has_value());

        CJKAnalyzer analyzer;
        IndexWriterConfig config(analyzer);
        auto writer = IndexWriter::open(*directory.value(), config);
        ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());

        const std::vector<std::string> texts{"中国人民热爱和平", "机器学习很有趣", "搜索引擊"};
        for (const std::string &text : texts) {
            Document doc;
            doc.add(std::make_unique<Field>("body", FieldValue{text}, body_type));
            ASSERT_TRUE(writer.value()->add_document(std::move(doc)).has_value());
        }
        ASSERT_TRUE(writer.value()->commit().has_value());
        ASSERT_TRUE(writer.value()->close().has_value());
    }

    auto directory = FSDirectory::open(path);
    ASSERT_TRUE(directory.has_value());
    auto reader = DirectoryReader::open(*directory.value());
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    CJKAnalyzer analyzer;
    QueryParser parser("body", analyzer);

    // The query text goes through the same bigram chain as the documents.
    EXPECT_EQ(count_hits(parser, searcher, "中国"), 1);
    EXPECT_EQ(count_hits(parser, searcher, "人民"), 1);
    EXPECT_EQ(count_hits(parser, searcher, "学习"), 1);
    EXPECT_EQ(count_hits(parser, searcher, "中国 AND 学习"), 0);
    EXPECT_EQ(count_hits(parser, searcher, "中国 OR 学习"), 2);
    // A character that no bigram covers matches nothing.
    EXPECT_EQ(count_hits(parser, searcher, "法国"), 0);

    ASSERT_TRUE(reader.value()->close().has_value());
}
