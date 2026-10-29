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

module lucene_core:ut.span_multi_term_query_wrapper;

import lucene_core;

using namespace lucene;

namespace {

std::unique_ptr<Directory> build_mtq_index(const std::filesystem::path &path, const std::vector<std::string> &texts) {
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

} // namespace

TEST(SpanMultiTermQueryWrapperTest, ExpandsIntoASpanOrAndKeepsPositions) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_span_mtq";
    auto directory = build_mtq_index(path, {"brown fox", "bronze fox", "black fox"});
    ASSERT_NE(directory, nullptr);

    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    IndexSearcher searcher(reader.value());

    auto prefix = std::make_shared<PrefixQuery>("body", "bro");
    auto wrapper = std::make_shared<SpanMultiTermQueryWrapper>(prefix);

    // The wrapper is a span query, so it can be nested in a span first query: the
    // expansion `brown|bronze` sits at position 0 of two documents.
    SpanFirstQuery first(wrapper, 1);
    auto count = searcher.count(first);
    ASSERT_TRUE(count.has_value()) << (count.has_value() ? "" : count.error().to_string());
    EXPECT_EQ(count.value(), 2);

    // Positions really come from the expanded spans: `brown` is at 0 and `fox` at
    // 1, so a near query over the wrapper and `fox` matches.
    auto near = SpanNearQuery::create({wrapper, std::make_shared<SpanTermQuery>(Term::from_text("body", "fox"))}, 0,
                                      true);
    ASSERT_TRUE(near.has_value()) << (near.has_value() ? "" : near.error().to_string());
    auto near_count = searcher.count(near.value());
    ASSERT_TRUE(near_count.has_value()) << (near_count.has_value() ? "" : near_count.error().to_string());
    EXPECT_EQ(near_count.value(), 2);

    EXPECT_TRUE(reader.value()->close().has_value());
}

TEST(SpanMultiTermQueryWrapperTest, WeightingTheWrapperDirectlyIsRejected) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_span_mtq_weight";
    auto directory = build_mtq_index(path, {"brown"});
    ASSERT_NE(directory, nullptr);

    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    SpanMultiTermQueryWrapper wrapper(std::make_shared<PrefixQuery>("body", "bro"));
    auto weight = wrapper.create_weight(searcher, ScoreMode::Complete, 1.0F);
    EXPECT_FALSE(weight.has_value());
    if (!weight.has_value()) {
        EXPECT_NE(weight.error().to_string().find("rewritten"), std::string::npos) << weight.error().to_string();
    }

    EXPECT_TRUE(reader.value()->close().has_value());
}

TEST(SpanMultiTermQueryWrapperTest, RewritesToASpanOrOfTheExpandedTerms) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_span_mtq_rewrite";
    auto directory = build_mtq_index(path, {"brown", "bronze", "black"});
    ASSERT_NE(directory, nullptr);

    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    auto wildcard = WildcardQuery::with_rewrite(Term::from_text("body", "bro*"), 10000, MultiTermRewriteMethod::kConstantScore);
    ASSERT_TRUE(wildcard.has_value()) << (wildcard.has_value() ? "" : wildcard.error().to_string());
    SpanMultiTermQueryWrapper wrapper(std::make_shared<WildcardQuery>(std::move(wildcard.value())));
    auto rewritten = wrapper.rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value()) << (rewritten.has_value() ? "" : rewritten.error().to_string());
    ASSERT_NE(rewritten.value(), nullptr);
    EXPECT_NE(dynamic_cast<const SpanOrQuery *>(rewritten.value().get()), nullptr);

    auto text = rewritten.value()->to_string("body");
    ASSERT_TRUE(text.has_value());
    // The expansion order follows the postings, so only the membership is asserted.
    EXPECT_NE(text.value().find("SpanTermQuery(brown)"), std::string::npos) << text.value();
    EXPECT_NE(text.value().find("SpanTermQuery(bronze)"), std::string::npos) << text.value();
    EXPECT_EQ(text.value().rfind("spanOr([", 0), 0u) << text.value();

    // A pattern that matches nothing becomes a no-match query rather than an empty
    // span or.
    SpanMultiTermQueryWrapper empty(std::make_shared<PrefixQuery>("body", "zzz"));
    auto empty_rewrite = empty.rewrite(searcher);
    ASSERT_TRUE(empty_rewrite.has_value());
    ASSERT_NE(empty_rewrite.value(), nullptr);
    EXPECT_NE(dynamic_cast<const MatchNoDocsQuery *>(empty_rewrite.value().get()), nullptr);

    EXPECT_TRUE(reader.value()->close().has_value());
}

TEST(SpanMultiTermQueryWrapperTest, FormatsAndCompares) {
    SpanMultiTermQueryWrapper query(std::make_shared<PrefixQuery>("body", "bro"));
    auto text = query.to_string("body");
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text.value(), "SpanMultiTermQueryWrapper(PrefixQuery(bro))");
    EXPECT_EQ(query.get_field(), "body");

    SpanMultiTermQueryWrapper same(std::make_shared<PrefixQuery>("body", "bro"));
    EXPECT_TRUE(query.equals(same));
    EXPECT_EQ(query.hash(), same.hash());

    SpanMultiTermQueryWrapper other(std::make_shared<PrefixQuery>("body", "bla"));
    EXPECT_FALSE(query.equals(other));
}
