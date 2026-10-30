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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.phrase_impacts;

import lucene_core;

using namespace lucene;

namespace {

/// An impacts list with a fixed block layout, so a test can describe exactly
/// what a term reports for the documents ahead of it.
class FixedImpacts final : public Impacts {
public:
    FixedImpacts(std::vector<int32_t> doc_id_upto, std::vector<std::vector<Impact>> levels)
            : doc_id_upto_(std::move(doc_id_upto)), levels_(std::move(levels)) {}

    [[nodiscard]] size_t num_levels() const override { return levels_.size(); }

    [[nodiscard]] int32_t get_doc_id_upto(size_t level) const override { return doc_id_upto_[level]; }

    [[nodiscard]] Result<std::span<const Impact>> get_impacts(size_t level) const override { return levels_[level]; }

private:
    std::vector<int32_t> doc_id_upto_;
    std::vector<std::vector<Impact>> levels_;
};

/// A term that reports `levels` for the documents up to `doc_id_upto`, and
/// records every shallow advance it receives.
class FixedImpactsEnum final : public ImpactsEnum {
public:
    FixedImpactsEnum(int64_t cost, std::vector<int32_t> doc_id_upto, std::vector<std::vector<Impact>> levels,
                     std::vector<int32_t> *shallow_advances = nullptr)
            : cost_(cost), doc_id_upto_(std::move(doc_id_upto)), levels_(std::move(levels)),
              shallow_advances_(shallow_advances), impacts_(doc_id_upto_, levels_) {}

    // ---- DocIdSetIterator: the tests never iterate these ----

    [[nodiscard]] int32_t doc_id() const override { return NO_MORE_DOCS; }
    [[nodiscard]] Result<int32_t> next_doc() override { return NO_MORE_DOCS; }
    [[nodiscard]] Result<int32_t> advance(int32_t /*target*/) override { return NO_MORE_DOCS; }
    [[nodiscard]] Result<int64_t> cost() const override { return cost_; }

    // ---- PostingsEnum: never read ----

    [[nodiscard]] Result<int32_t> freq() override { return 0; }
    [[nodiscard]] Result<int32_t> next_position() override { return -1; }
    [[nodiscard]] Result<int32_t> start_offset() const override { return -1; }
    [[nodiscard]] Result<int32_t> end_offset() const override { return -1; }
    [[nodiscard]] Result<std::optional<std::span<const uint8_t>>> get_payload() const override {
        return std::optional<std::span<const uint8_t>>{};
    }

    // ---- ImpactsSource ----

    [[nodiscard]] VoidResult advance_shallow(int32_t target) override {
        if (shallow_advances_ != nullptr) {
            shallow_advances_->push_back(target);
        }
        return {};
    }
    [[nodiscard]] Result<Impacts *> get_impacts() override { return &impacts_; }

private:
    int64_t cost_;
    std::vector<int32_t> doc_id_upto_;
    std::vector<std::vector<Impact>> levels_;
    std::vector<int32_t> *shallow_advances_;
    FixedImpacts impacts_;
};

/// The pair `(freq, norm)` the merged impacts report, for terser assertions.
std::vector<std::pair<int32_t, int64_t>> pairs(std::span<const Impact> impacts) {
    std::vector<std::pair<int32_t, int64_t>> result;
    result.reserve(impacts.size());
    for (const Impact &impact : impacts) {
        result.emplace_back(impact.freq, impact.norm);
    }
    return result;
}

// --------------------------------------------------------------- the index

constexpr std::string_view kBody = "body";

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqsAndPositions).has_value());
    return type;
}

/// Builds `count` documents whose `body` holds `copies` of the phrase `a b`;
/// every fourth document separates the terms, so it matches both terms but not
/// the phrase.
void build_index(Directory &directory, int32_t count) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());

    for (int32_t doc = 0; doc < count; ++doc) {
        std::string text;
        const int32_t copies = doc + 1;
        for (int32_t copy = 0; copy < copies; ++copy) {
            text += "a b ";
        }
        if (doc % 4 == 3) {
            // The terms are there, but the phrase is not.
            text += "a x b";
        }
        Document document;
        document.add(std::make_unique<Field>(std::string(kBody), FieldValue{std::move(text)}, text_type()));
        ASSERT_TRUE(writer.value()->add_document(std::move(document)).has_value());
    }
    ASSERT_TRUE(writer.value()->close().has_value());
}

Result<PhraseQuery> phrase_query() { return PhraseQuery::from_term_texts(std::string(kBody), {"a", "b"}); }

/// Every document matching `query`, scored without any skipping, ordered the way
/// a collector orders hits: best score first, lowest document on a tie.
Result<std::vector<ScoreDoc>> exhaustive_hits(IndexSearcher &searcher, const Query &query) {
    auto weight = searcher.create_weight(query, ScoreMode::Complete, 1.0F);
    if (!weight.has_value()) {
        return std::unexpected(std::move(weight.error()));
    }
    const auto leaves = searcher.leaves();
    if (!leaves.has_value()) {
        return std::unexpected(std::move(leaves.error()));
    }
    std::vector<ScoreDoc> hits;
    for (const LeafReaderContext &context : leaves.value()) {
        auto scorer = weight.value()->scorer(context, searcher);
        if (!scorer.has_value()) {
            return std::unexpected(std::move(scorer.error()));
        }
        if (scorer.value() == nullptr) {
            continue;
        }
        while (true) {
            auto doc = scorer.value()->next_doc();
            if (!doc.has_value()) {
                return std::unexpected(std::move(doc.error()));
            }
            if (doc.value() == NO_MORE_DOCS) {
                break;
            }
            auto score = scorer.value()->score();
            if (!score.has_value()) {
                return std::unexpected(std::move(score.error()));
            }
            hits.push_back(ScoreDoc{score.value(), doc.value() + context.doc_base(), 0});
        }
    }
    std::sort(hits.begin(), hits.end(), [](const ScoreDoc &left, const ScoreDoc &right) {
        if (left.score != right.score) {
            return left.score > right.score;
        }
        return left.doc < right.doc;
    });
    return hits;
}

} // namespace

// ------------------------------------------------------------- the merge

TEST(PhraseImpactsTest, MergesTheFrequenciesOfAllTerms) {
    FixedImpactsEnum first(4, {9}, {{{2, 5}, {4, 3}}});
    FixedImpactsEnum second(9, {9}, {{{3, 4}, {6, 2}}});
    PhraseImpacts phrase_impacts({&first, &second});

    auto impacts = phrase_impacts.get_impacts();
    ASSERT_TRUE(impacts.has_value()) << impacts.error().to_string();
    ASSERT_EQ(impacts.value()->num_levels(), 1U);
    EXPECT_EQ(impacts.value()->get_doc_id_upto(0), 9);

    // The phrase never occurs more often than either term, and the largest norm
    // any of them reports still has to be assumed for every frequency, so the
    // two lists collapse into the single most pessimistic pair.
    auto merged = impacts.value()->get_impacts(0);
    ASSERT_TRUE(merged.has_value()) << merged.error().to_string();
    EXPECT_EQ(pairs(merged.value()), (std::vector<std::pair<int32_t, int64_t>>{{4, 5}}));
}

TEST(PhraseImpactsTest, DropsTermsThatNoLongerCoverTheBlock) {
    FixedImpactsEnum first(4, {3, 9}, {{{1, 5}, {2, 5}}, {{4, 5}}});
    FixedImpactsEnum second(9, {3}, {{{1, 1}}});
    PhraseImpacts phrase_impacts({&first, &second});

    auto impacts = phrase_impacts.get_impacts();
    ASSERT_TRUE(impacts.has_value()) << impacts.error().to_string();
    ASSERT_EQ(impacts.value()->num_levels(), 2U);

    // On the first block both terms report: the phrase can occur as often as the
    // rarest of them allows, and the longest length any of them reports has to
    // be assumed, so the pair is the pessimistic `(1, 5)`.
    auto first_block = impacts.value()->get_impacts(0);
    ASSERT_TRUE(first_block.has_value());
    EXPECT_EQ(pairs(first_block.value()), (std::vector<std::pair<int32_t, int64_t>>{{1, 5}}));

    // The second term has no impacts for the second block, so the lead's list is
    // an upper bound of the phrase on its own.
    auto second_block = impacts.value()->get_impacts(1);
    ASSERT_TRUE(second_block.has_value());
    EXPECT_EQ(pairs(second_block.value()), (std::vector<std::pair<int32_t, int64_t>>{{4, 5}}));
}

TEST(PhraseImpactsTest, IgnoresTermsWithoutUsefulImpacts) {
    // A postings list that knows nothing about its documents reports the
    // "no impact" pair, which must not widen the bound of the term that does.
    constexpr int32_t kNoFreq = std::numeric_limits<int32_t>::max();
    FixedImpactsEnum first(4, {9}, {{{2, 5}}});
    FixedImpactsEnum unknown(9, {9}, {{{kNoFreq, 1}}});
    PhraseImpacts phrase_impacts({&first, &unknown});

    auto impacts = phrase_impacts.get_impacts();
    ASSERT_TRUE(impacts.has_value()) << impacts.error().to_string();
    auto merged = impacts.value()->get_impacts(0);
    ASSERT_TRUE(merged.has_value());
    EXPECT_EQ(pairs(merged.value()), (std::vector<std::pair<int32_t, int64_t>>{{2, 5}}));
}

TEST(PhraseImpactsTest, ReportsNoImpactWhenNothingIsKnown) {
    constexpr int32_t kNoFreq = std::numeric_limits<int32_t>::max();
    FixedImpactsEnum first(4, {9}, {{{kNoFreq, 1}}});
    FixedImpactsEnum second(9, {9}, {{{kNoFreq, 1}}});
    PhraseImpacts phrase_impacts({&first, &second});

    auto impacts = phrase_impacts.get_impacts();
    ASSERT_TRUE(impacts.has_value()) << impacts.error().to_string();
    auto merged = impacts.value()->get_impacts(0);
    ASSERT_TRUE(merged.has_value());
    EXPECT_EQ(pairs(merged.value()), (std::vector<std::pair<int32_t, int64_t>>{{kNoFreq, 1}}));
}

TEST(PhraseImpactsTest, UsesTheCheapestTermForTheBlockLayout) {
    // The cheaper list reports a coarse second level; the other one stops at a
    // much earlier document, so its impacts only cover the first block.
    FixedImpactsEnum expensive(100, {2, 5}, {{{1, 9}}, {{2, 9}}});
    FixedImpactsEnum cheap(3, {9}, {{{1, 4}}});
    PhraseImpacts phrase_impacts({&expensive, &cheap});

    auto impacts = phrase_impacts.get_impacts();
    ASSERT_TRUE(impacts.has_value()) << impacts.error().to_string();
    EXPECT_EQ(impacts.value()->num_levels(), 1U);
    EXPECT_EQ(impacts.value()->get_doc_id_upto(0), 9);
}

TEST(PhraseImpactsTest, ForwardsShallowAdvanceToEveryTerm) {
    std::vector<int32_t> first_advances;
    std::vector<int32_t> second_advances;
    FixedImpactsEnum first(4, {9}, {{{1, 5}}}, &first_advances);
    FixedImpactsEnum second(9, {9}, {{{1, 5}}}, &second_advances);
    PhraseImpacts phrase_impacts({&first, &second});

    ASSERT_TRUE(phrase_impacts.advance_shallow(7).has_value());
    EXPECT_EQ(first_advances, (std::vector<int32_t>{7}));
    EXPECT_EQ(second_advances, (std::vector<int32_t>{7}));
}

// --------------------------------------------------------- the phrase scorer

TEST(PhraseImpactsTest, TopScoresScorerReportsImpactBasedMaximumScores) {
    ByteBuffersDirectory directory;
    build_index(directory, 40);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    IndexSearcher searcher(reader.value());
    auto query = phrase_query();
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    const auto leaves = searcher.leaves();
    ASSERT_TRUE(leaves.has_value());
    const LeafReaderContext &context = leaves.value().front();

    auto skipping = searcher.create_weight(query.value(), ScoreMode::TopScores, 1.0F);
    ASSERT_TRUE(skipping.has_value()) << skipping.error().to_string();
    auto skipping_scorer = skipping.value()->scorer(context, searcher);
    ASSERT_TRUE(skipping_scorer.has_value());
    ASSERT_NE(skipping_scorer.value(), nullptr);

    // The impacts of the phrase cover a real block, and bound its scores from
    // above; the whole postings list is one block here.
    auto block_end = skipping_scorer.value()->advance_shallow(0);
    ASSERT_TRUE(block_end.has_value()) << block_end.error().to_string();
    EXPECT_GE(block_end.value(), 0);

    auto bound = skipping_scorer.value()->get_max_score(block_end.value());
    ASSERT_TRUE(bound.has_value()) << bound.error().to_string();
    EXPECT_TRUE(std::isfinite(bound.value()));

    const auto oracle = exhaustive_hits(searcher, query.value());
    ASSERT_TRUE(oracle.has_value()) << oracle.error().to_string();
    ASSERT_FALSE(oracle.value().empty());
    EXPECT_GE(bound.value(), oracle.value().front().score);

    // Without scoring there is nothing to skip, and the bound is the trivial one.
    auto plain = searcher.create_weight(query.value(), ScoreMode::CompleteNoScores, 1.0F);
    ASSERT_TRUE(plain.has_value()) << plain.error().to_string();
    auto plain_scorer = plain.value()->scorer(context, searcher);
    ASSERT_TRUE(plain_scorer.has_value());
    ASSERT_NE(plain_scorer.value(), nullptr);
    auto plain_bound = plain_scorer.value()->get_max_score(searcher.reader().max_doc().value());
    ASSERT_TRUE(plain_bound.has_value());
    EXPECT_EQ(plain_bound.value(), std::numeric_limits<float>::infinity());
}

TEST(PhraseImpactsTest, SkipsOnlyDocumentsBelowTheCompetitiveScore) {
    ByteBuffersDirectory directory;
    build_index(directory, 40);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());
    auto query = phrase_query();
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    const auto oracle = exhaustive_hits(searcher, query.value());
    ASSERT_TRUE(oracle.has_value()) << oracle.error().to_string();
    ASSERT_GT(oracle.value().size(), 10U);

    auto weight = searcher.create_weight(query.value(), ScoreMode::TopScores, 1.0F);
    ASSERT_TRUE(weight.has_value());
    auto scorer = weight.value()->scorer(searcher.leaves().value().front(), searcher);
    ASSERT_TRUE(scorer.has_value());
    ASSERT_NE(scorer.value(), nullptr);

    // Ask for the score of a document in the middle of the ranking: everything
    // below it can be skipped without changing the answer.
    const float threshold = oracle.value()[oracle.value().size() / 2].score;
    ASSERT_TRUE(scorer.value()->set_min_competitive_score(threshold).has_value());

    std::map<int32_t, float> expected;
    for (const ScoreDoc &hit : oracle.value()) {
        expected.emplace(hit.doc, hit.score);
    }

    std::map<int32_t, float> visited;
    while (true) {
        auto doc = scorer.value()->next_doc();
        ASSERT_TRUE(doc.has_value()) << doc.error().to_string();
        if (doc.value() == NO_MORE_DOCS) {
            break;
        }
        auto score = scorer.value()->score();
        ASSERT_TRUE(score.has_value());
        // Skipping never invents or drops a match: every visited document is a
        // real hit with the score the exhaustive scan gives it.
        const auto found = expected.find(doc.value());
        ASSERT_NE(found, expected.end()) << "unexpected document " << doc.value();
        EXPECT_FLOAT_EQ(score.value(), found->second);
        visited.emplace(doc.value(), score.value());
    }

    // Whole blocks are skipped, so fewer documents are visited...
    EXPECT_LT(visited.size(), oracle.value().size());
    EXPECT_FALSE(visited.empty());
    // ...but every document that could compete is still visited.
    for (const ScoreDoc &hit : oracle.value()) {
        if (hit.score >= threshold) {
            EXPECT_EQ(visited.count(hit.doc), 1U) << "competitive document " << hit.doc << " was skipped";
        }
    }
}

TEST(PhraseImpactsTest, TopHitsMatchTheExhaustiveRanking) {
    ByteBuffersDirectory directory;
    build_index(directory, 60);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());
    auto query = phrase_query();
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    const auto oracle = exhaustive_hits(searcher, query.value());
    ASSERT_TRUE(oracle.has_value()) << oracle.error().to_string();

    for (const int32_t top : {1, 5, 20, 100}) {
        auto hits = searcher.search(query.value(), top);
        ASSERT_TRUE(hits.has_value()) << hits.error().to_string();
        const auto &docs = hits.value().score_docs;
        ASSERT_EQ(docs.size(), std::min(static_cast<size_t>(top), oracle.value().size()));
        for (size_t i = 0; i < docs.size(); ++i) {
            EXPECT_EQ(docs[i].doc, oracle.value()[i].doc) << "top " << top << " hit " << i;
            EXPECT_FLOAT_EQ(docs[i].score, oracle.value()[i].score) << "top " << top << " hit " << i;
        }
        EXPECT_EQ(hits.value().total_hits.value, static_cast<int64_t>(oracle.value().size()));
    }
}
