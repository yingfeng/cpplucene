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

// A term range query rewritten onto doc values, on a field that has a doc values
// skip index: the skipper may prune whole blocks, and it may prove that no
// document matches at all, but the hits have to be the ones a scan finds.

module;

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.doc_values_skip_index;

import lucene_core;

using namespace lucene;

namespace {

constexpr int32_t DOC_COUNT = 120;
constexpr std::string_view kTag = "tag";

/// The tag of document `doc`: `tag00` .. `tag29`, so a range picks a band of them.
std::string tag_of(int32_t doc) {
    const int32_t value = doc % 30;
    return "tag" + (value < 10 ? std::string("0") : std::string("")) + std::to_string(value);
}

/// Writes `DOC_COUNT` documents whose `tag` is a keyword field with the given
/// skip index.
void build(Directory &directory, DocValuesSkipIndexType skip_index) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());
    for (int32_t doc = 0; doc < DOC_COUNT; ++doc) {
        Document document;
        auto tag = KeywordField::create(std::string(kTag), tag_of(doc), /*stored=*/true, skip_index);
        ASSERT_TRUE(tag.has_value()) << tag.error().to_string();
        document.add(std::make_unique<Field>(std::move(tag.value())));
        ASSERT_TRUE(writer.value()->add_document(std::move(document)).has_value());
    }
    ASSERT_TRUE(writer.value()->close().has_value());
}

/// The documents a range over the tags matches, by scanning the source data.
std::vector<int32_t> expected(const std::string &lower, const std::string &upper) {
    std::vector<int32_t> docs;
    for (int32_t doc = 0; doc < DOC_COUNT; ++doc) {
        const std::string tag = tag_of(doc);
        if (tag >= lower && tag <= upper) {
            docs.push_back(doc);
        }
    }
    return docs;
}

std::vector<int32_t> hits_of(const TopDocs &top) {
    std::vector<int32_t> docs;
    for (const ScoreDoc &hit : top.score_docs) {
        docs.push_back(hit.doc);
    }
    std::sort(docs.begin(), docs.end());
    return docs;
}

/// Runs the range `[lower, upper]` with the doc values rewrite method.
///
/// The doc values rewrite keeps the query alive while its weight runs, so it has
/// to be owned by a shared pointer.
Result<TopDocs> search_range(IndexSearcher &searcher, const std::string &lower, const std::string &upper) {
    auto built = TermRangeQuery::with_string_range_rewrite(std::string(kTag), lower, upper, true, true,
                                                           MultiTermRewriteMethod::kDocValues);
    if (!built.has_value()) {
        return std::unexpected(std::move(built.error()));
    }
    const auto query = std::make_shared<TermRangeQuery>(std::move(built.value()));
    return searcher.search(*query, DOC_COUNT);
}

} // namespace

TEST(DocValuesSkipIndexTest, ARangeMatchesTheSameDocumentsWithASkipIndex) {
    ByteBuffersDirectory with_skip;
    build(with_skip, DocValuesSkipIndexType::Range);
    auto reader = DirectoryReader::open(with_skip);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    IndexSearcher searcher(reader.value());

    // The test only means something when the field really has a skip index.
    LeafReader *leaf = reader.value()->leaves().value().front().reader();
    auto infos = leaf->field_infos();
    ASSERT_TRUE(infos.has_value());
    const FieldInfo *info = infos.value()->by_name(std::string(kTag));
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->doc_values_skip_index, DocValuesSkipIndexType::Range);
    auto skipper = leaf->get_doc_values_skipper(std::string(kTag));
    ASSERT_TRUE(skipper.has_value()) << skipper.error().to_string();
    ASSERT_NE(skipper.value(), nullptr) << "the writer has to record a skip index";

    for (const auto &[lower, upper] : std::vector<std::pair<std::string, std::string>>{
                 {"tag00", "tag09"}, {"tag10", "tag19"}, {"tag05", "tag25"}, {"tag07", "tag07"}}) {
        auto hits = search_range(searcher, lower, upper);
        ASSERT_TRUE(hits.has_value()) << lower << ".." << upper << ": " << hits.error().to_string();
        EXPECT_EQ(hits_of(hits.value()), expected(lower, upper)) << lower << ".." << upper;
    }
}

TEST(DocValuesSkipIndexTest, ARangeWithoutASkipIndexMatchesTheSameDocuments) {
    ByteBuffersDirectory without_skip;
    build(without_skip, DocValuesSkipIndexType::None);
    auto reader = DirectoryReader::open(without_skip);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    IndexSearcher searcher(reader.value());

    auto hits = search_range(searcher, "tag03", "tag12");
    ASSERT_TRUE(hits.has_value()) << hits.error().to_string();
    EXPECT_EQ(hits_of(hits.value()), expected("tag03", "tag12"));
}

TEST(DocValuesSkipIndexTest, ARangeThatCannotMatchAnythingIsEmpty) {
    ByteBuffersDirectory directory;
    build(directory, DocValuesSkipIndexType::Range);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    IndexSearcher searcher(reader.value());

    // Outside the dictionary the field holds: the skipper's global range proves
    // that no document can match, without walking any document.
    auto above = search_range(searcher, "zzz", "zzz9");
    ASSERT_TRUE(above.has_value()) << above.error().to_string();
    EXPECT_TRUE(above.value().score_docs.empty());
    EXPECT_EQ(above.value().total_hits.value, 0U);

    auto below = search_range(searcher, "aaa", "aaa9");
    ASSERT_TRUE(below.has_value()) << below.error().to_string();
    EXPECT_TRUE(below.value().score_docs.empty());
}
