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

// A boolean query whose positive side is a conjunction is scored block-wise, and
// its `MUST NOT` clauses are applied by `ReqExclBulkScorer`; the hits have to be
// exactly the documents the positive clauses match and the exclusions do not.

module;

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.req_excl_bulk_scorer;

import lucene_core;

using namespace lucene;

namespace {

constexpr int32_t kDocs = 60;

FieldType body_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    return type;
}

/// Document `doc` holds "alpha" always, "rare" when `doc % 3 == 0`, "beta" when
/// `doc % 5 == 0` and "omega" when `doc % 7 == 0`.
void build_index(Directory &directory) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_max_buffered_docs(11); // several segments
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());
    for (int32_t doc = 0; doc < kDocs; ++doc) {
        Document document;
        document.add(std::make_unique<Field>("alpha", FieldValue{std::string("alpha")}, body_type()));
        if (doc % 3 == 0) {
            document.add(std::make_unique<Field>("rare", FieldValue{std::string("rare")}, body_type()));
        }
        if (doc % 5 == 0) {
            document.add(std::make_unique<Field>("beta", FieldValue{std::string("beta")}, body_type()));
        }
        if (doc % 7 == 0) {
            document.add(std::make_unique<Field>("omega", FieldValue{std::string("omega")}, body_type()));
        }
        ASSERT_TRUE(writer.value()->add_document(std::move(document)).has_value());
    }
    ASSERT_TRUE(writer.value()->close().has_value());
}

/// `+alpha +rare -beta [-omega]`: two required clauses, so the positive side can be
/// scored as a conjunction.
std::shared_ptr<Query> positive_query(bool with_omega) {
    BooleanQuery::Builder builder;
    EXPECT_TRUE(builder.add(std::make_shared<TermQuery>(Term::from_text("alpha", "alpha")), Occur::Must).has_value());
    EXPECT_TRUE(builder.add(std::make_shared<TermQuery>(Term::from_text("rare", "rare")), Occur::Must).has_value());
    EXPECT_TRUE(builder.add(std::make_shared<TermQuery>(Term::from_text("beta", "beta")), Occur::MustNot).has_value());
    if (with_omega) {
        EXPECT_TRUE(
                builder.add(std::make_shared<TermQuery>(Term::from_text("omega", "omega")), Occur::MustNot).has_value());
    }
    auto built = builder.build();
    EXPECT_TRUE(built.has_value()) << (built.has_value() ? "" : built.error().to_string());
    return std::make_shared<BooleanQuery>(std::move(built.value()));
}

/// The documents the query has to return, from the source data.
std::vector<int32_t> expected_docs(bool with_omega) {
    std::vector<int32_t> docs;
    for (int32_t doc = 0; doc < kDocs; ++doc) {
        if (doc % 3 != 0) {
            continue;
        }
        if (doc % 5 == 0) {
            continue;
        }
        if (with_omega && doc % 7 == 0) {
            continue;
        }
        docs.push_back(doc);
    }
    return docs;
}

std::vector<int32_t> docs_of(const TopDocs &top) {
    std::vector<int32_t> docs;
    for (const ScoreDoc &hit : top.score_docs) {
        docs.push_back(hit.doc);
    }
    std::sort(docs.begin(), docs.end());
    return docs;
}

} // namespace

TEST(ReqExclBulkScorerTest, ExclusionsDropExactlyTheDocumentsTheyMatch) {
    ByteBuffersDirectory directory;
    build_index(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    const IndexSearcher searcher(reader.value());
    ASSERT_GT(reader.value()->leaves().value().size(), 1U);

    for (const bool with_omega : {false, true}) {
        const auto query = positive_query(with_omega);
        const std::vector<int32_t> expected = expected_docs(with_omega);
        ASSERT_FALSE(expected.empty());

        auto counted = searcher.count(*query);
        ASSERT_TRUE(counted.has_value()) << counted.error().to_string();
        EXPECT_EQ(static_cast<size_t>(counted.value()), expected.size()) << with_omega;

        auto top = searcher.search(*query, kDocs);
        ASSERT_TRUE(top.has_value()) << top.error().to_string();
        EXPECT_EQ(static_cast<size_t>(top.value().total_hits.value), expected.size()) << with_omega;
        EXPECT_EQ(docs_of(top.value()), expected) << with_omega;
    }
}

TEST(ReqExclBulkScorerTest, ASingleExclusionStillKeepsEveryHit) {
    // The top-k path stops early, so the exclusion has to be applied to the
    // documents that are actually collected.
    ByteBuffersDirectory directory;
    build_index(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    const IndexSearcher searcher(reader.value());
    const auto query = positive_query(true);
    const std::vector<int32_t> expected = expected_docs(true);
    ASSERT_GE(expected.size(), 3U);

    auto top = searcher.search(*query, 3);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    ASSERT_EQ(top.value().score_docs.size(), 3U);
    for (const ScoreDoc &hit : top.value().score_docs) {
        EXPECT_NE(std::find(expected.begin(), expected.end(), hit.doc), expected.end()) << hit.doc;
    }
}
