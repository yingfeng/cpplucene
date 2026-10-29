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

module lucene_core:ut.span_not_query;

import lucene_core;

using namespace lucene;

namespace {

std::unique_ptr<Directory> build_not_index(const std::filesystem::path &path, const std::vector<std::string> &texts) {
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

std::shared_ptr<SpanQuery> not_term(std::string_view field, std::string_view text) {
    return std::make_shared<SpanTermQuery>(Term::from_text(std::string(field), text));
}

int32_t count_not(IndexSearcher &searcher, std::shared_ptr<SpanQuery> include, std::shared_ptr<SpanQuery> exclude,
                  int32_t pre, int32_t post) {
    auto query = SpanNotQuery::create(std::move(include), std::move(exclude), pre, post);
    EXPECT_TRUE(query.has_value()) << (query.has_value() ? "" : query.error().to_string());
    if (!query.has_value()) {
        return -1;
    }
    auto count = searcher.count(query.value());
    EXPECT_TRUE(count.has_value()) << (count.has_value() ? "" : count.error().to_string());
    return count.value_or(-1);
}

} // namespace

TEST(SpanNotQueryTest, ExcludesSpansInsideTheWindow) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_span_not";
    auto directory = build_not_index(path, {"quick brown", "brown fox"});
    ASSERT_NE(directory, nullptr);

    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    IndexSearcher searcher(reader.value());

    // Without a window (`pre = post = 0`) the neighbours do not overlap, so both
    // documents keep their `brown`.
    EXPECT_EQ(count_not(searcher, not_term("body", "brown"), not_term("body", "quick"), 0, 0), 2);

    // `pre = 1` reaches back one position: in doc 0 `quick` ends right before
    // `brown` starts, so the pair is excluded and only doc 1 survives.
    EXPECT_EQ(count_not(searcher, not_term("body", "brown"), not_term("body", "quick"), 1, 0), 1);

    // The mirror image on the post side: `post = 1` reaches forward one position,
    // which excludes `brown` in both documents (`quick` in doc 0, `fox` in doc 1).
    EXPECT_EQ(count_not(searcher, not_term("body", "brown"), not_term("body", "fox"), 0, 1), 1);

    EXPECT_TRUE(reader.value()->close().has_value());
}

TEST(SpanNotQueryTest, KeepsDocumentsWithoutAnExclusion) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_span_not_simple";
    auto directory = build_not_index(path, {"brown", "quick brown"});
    ASSERT_NE(directory, nullptr);

    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    // Doc 0 has no `quick` at all, so its `brown` stays.
    EXPECT_EQ(count_not(searcher, not_term("body", "brown"), not_term("body", "quick"), 1, 0), 1);

    // Excluding a term that no document holds keeps everything.
    EXPECT_EQ(count_not(searcher, not_term("body", "brown"), not_term("body", "zebra"), 5, 5), 2);

    EXPECT_TRUE(reader.value()->close().has_value());
}

TEST(SpanNotQueryTest, RejectsInvalidClauses) {
    EXPECT_FALSE(SpanNotQuery::create(nullptr, not_term("body", "a"), 0, 0).has_value());
    EXPECT_FALSE(SpanNotQuery::create(not_term("body", "a"), not_term("title", "b"), 0, 0).has_value());
}

TEST(SpanNotQueryTest, FormatsAndCompares) {
    auto query = SpanNotQuery::create(not_term("body", "brown"), not_term("body", "quick"), 1, 2);
    ASSERT_TRUE(query.has_value());
    auto text = query.value().to_string("body");
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text.value(), "spanNot(SpanTermQuery(brown), SpanTermQuery(quick), 1, 2)");

    auto same = SpanNotQuery::create(not_term("body", "brown"), not_term("body", "quick"), 1, 2);
    ASSERT_TRUE(same.has_value());
    EXPECT_TRUE(query.value().equals(same.value()));
    EXPECT_EQ(query.value().hash(), same.value().hash());

    auto other = SpanNotQuery::create(not_term("body", "brown"), not_term("body", "quick"), 2, 2);
    ASSERT_TRUE(other.has_value());
    EXPECT_FALSE(query.value().equals(other.value()));
}
