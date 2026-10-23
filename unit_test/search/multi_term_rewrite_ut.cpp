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
#include <expected>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.multi_term_rewrite;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kField = "body";

/// [`PostingsEnum`] over a fixed list of document ids, frequency 1 each.
class DocsPostingsEnum final : public PostingsEnum {
public:
    explicit DocsPostingsEnum(std::vector<int32_t> docs) : docs_(std::move(docs)) {}

    [[nodiscard]] int32_t doc_id() const override {
        if (index_ < 0) {
            return -1;
        }
        return index_ < static_cast<int32_t>(docs_.size()) ? docs_[static_cast<size_t>(index_)] : NO_MORE_DOCS;
    }

    [[nodiscard]] Result<int32_t> next_doc() override {
        ++index_;
        return doc_id();
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        while (doc_id() != NO_MORE_DOCS && doc_id() < target) {
            ++index_;
        }
        return doc_id();
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(docs_.size()); }

    [[nodiscard]] Result<int32_t> freq() override { return 1; }
    [[nodiscard]] Result<int32_t> next_position() override {
        return err(LuceneError::unsupported_operation("positions are not indexed"));
    }
    [[nodiscard]] Result<int32_t> start_offset() const override {
        return err(LuceneError::unsupported_operation("offsets are not indexed"));
    }
    [[nodiscard]] Result<int32_t> end_offset() const override {
        return err(LuceneError::unsupported_operation("offsets are not indexed"));
    }
    [[nodiscard]] Result<std::optional<std::span<const uint8_t>>> get_payload() const override {
        return std::nullopt;
    }

private:
    std::vector<int32_t> docs_;
    int32_t index_{-1};
};

/// [`TermsEnum`] over the fake dictionary, with postings and without attributes.
class MapTermsEnum final : public TermsEnum {
public:
    explicit MapTermsEnum(std::map<std::string, std::vector<int32_t>> terms) : terms_(std::move(terms)) {}

    [[nodiscard]] Result<const BytesRef *> next() override {
        if (!pos_.has_value()) {
            if (terms_.empty()) {
                return static_cast<const BytesRef *>(nullptr);
            }
            pos_ = 0;
            return &term_at(0);
        }
        if (pos_.value() + 1 >= terms_.size()) {
            pos_.reset();
            return static_cast<const BytesRef *>(nullptr);
        }
        pos_ = pos_.value() + 1;
        return &term_at(pos_.value());
    }

    [[nodiscard]] Result<const BytesRef *> term() const override {
        if (!pos_.has_value()) {
            return err(LuceneError::illegal_state("unpositioned"));
        }
        return &term_at(pos_.value());
    }

    [[nodiscard]] Result<int64_t> ord() const override { return static_cast<int64_t>(pos_.value_or(0)); }

    [[nodiscard]] Result<int32_t> doc_freq() override {
        return static_cast<int32_t>(docs_at(pos_.value_or(0)).size());
    }
    [[nodiscard]] Result<int64_t> total_term_freq() override { return doc_freq().value(); }

    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>> postings_with_flags(std::unique_ptr<PostingsEnum> /*reuse*/,
                                                                           int32_t /*flags*/) override {
        return std::unique_ptr<PostingsEnum>(new DocsPostingsEnum(docs_at(pos_.value_or(0))));
    }

    [[nodiscard]] Result<std::unique_ptr<ImpactsEnum>> impacts(int32_t /*flags*/) override {
        return err(LuceneError::unsupported_operation("no impacts"));
    }

    [[nodiscard]] Result<AttributeSource *> attributes() override {
        return err(LuceneError::unsupported_operation("no attributes"));
    }
    [[nodiscard]] Result<AttributeSource *> attributes_mut() override {
        return err(LuceneError::unsupported_operation("no attributes"));
    }

    [[nodiscard]] Result<SeekStatus> seek_ceil(std::span<const uint8_t> target) override {
        const std::string text(reinterpret_cast<const char *>(target.data()), target.size());
        for (size_t i = 0; i < terms_.size(); ++i) {
            const auto &candidate = std::next(terms_.begin(), static_cast<std::ptrdiff_t>(i))->first;
            if (candidate < text) {
                continue;
            }
            pos_ = i;
            return candidate == text ? SeekStatus::Found : SeekStatus::NotFound;
        }
        pos_.reset();
        return SeekStatus::End;
    }

    [[nodiscard]] Result<bool> seek_exact(std::span<const uint8_t> target) override {
        auto status = seek_ceil(target);
        if (!status.has_value()) {
            return std::unexpected(std::move(status.error()));
        }
        return status.value() == SeekStatus::Found;
    }

    [[nodiscard]] Result<bool> prepare_seek_exact(std::span<const uint8_t> target) override {
        return seek_exact(target);
    }

    [[nodiscard]] Result<bool> get_prepare_seek_exact_status(std::span<const uint8_t> target) override {
        return seek_exact(target);
    }

    [[nodiscard]] VoidResult seek_exact_with_ord(int64_t ord) override {
        if (ord < 0 || static_cast<size_t>(ord) >= terms_.size()) {
            return err(LuceneError::illegal_argument("ord out of range"));
        }
        pos_ = static_cast<size_t>(ord);
        return {};
    }

    [[nodiscard]] VoidResult seek_exact_with_state(std::span<const uint8_t> /*term*/,
                                                   const TermStateEnum &state) override {
        auto ord = state.ord();
        if (!ord.has_value()) {
            return std::unexpected(std::move(ord.error()));
        }
        return seek_exact_with_ord(ord.value());
    }

    [[nodiscard]] Result<TermStateEnum> term_state() override { return TermStateEnum(OrdTermState{ord().value()}); }

private:
    [[nodiscard]] const std::vector<int32_t> &docs_at(size_t index) const {
        return std::next(terms_.begin(), static_cast<std::ptrdiff_t>(index))->second;
    }

    [[nodiscard]] const BytesRef &term_at(size_t index) const {
        cache_ = BytesRef::from_string(std::next(terms_.begin(), static_cast<std::ptrdiff_t>(index))->first);
        return cache_;
    }

    std::map<std::string, std::vector<int32_t>> terms_;
    std::optional<size_t> pos_{};
    mutable BytesRef cache_{};
};

/// [`Terms`] over the fake dictionary.
class MapTerms final : public Terms {
public:
    MapTerms(std::map<std::string, std::vector<int32_t>> terms, int32_t doc_count)
            : terms_(std::move(terms)), doc_count_(doc_count) {}

    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> iterator() const override {
        return std::unique_ptr<TermsEnum>(new MapTermsEnum(terms_));
    }

    [[nodiscard]] Result<int64_t> size() const override { return static_cast<int64_t>(terms_.size()); }

    [[nodiscard]] Result<int64_t> sum_total_term_freq() const override {
        int64_t sum = 0;
        for (const auto &[text, docs] : terms_) {
            sum += static_cast<int64_t>(docs.size());
        }
        return sum;
    }

    [[nodiscard]] Result<int64_t> sum_doc_freq() const override { return sum_total_term_freq(); }
    [[nodiscard]] Result<int32_t> doc_count() const override { return doc_count_; }
    [[nodiscard]] bool has_freqs() const override { return true; }
    [[nodiscard]] bool has_offsets() const override { return false; }
    [[nodiscard]] bool has_positions() const override { return false; }
    [[nodiscard]] bool has_payloads() const override { return false; }

private:
    std::map<std::string, std::vector<int32_t>> terms_;
    int32_t doc_count_;
};

/// A [`LeafReader`] with one terms field and nothing else.
class TermsLeafReader final : public LeafReader {
public:
    TermsLeafReader(std::string field, std::map<std::string, std::vector<int32_t>> terms, int32_t doc_count,
                    int32_t max_doc)
            : field_(std::move(field)), terms_(std::move(terms)), doc_count_(doc_count), max_doc_(max_doc) {}

    [[nodiscard]] Result<int32_t> max_doc() const override { return max_doc_; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return max_doc_; }

    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override { return nullptr; }
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override { return nullptr; }

    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view field) override {
        if (field != field_) {
            return std::unique_ptr<Terms>{};
        }
        return std::unique_ptr<Terms>(new MapTerms(terms_, doc_count_));
    }

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric_doc_values(std::string_view /*field*/) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> get_binary_doc_values(std::string_view /*field*/) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted_doc_values(std::string_view /*field*/) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>>
    get_sorted_numeric_doc_values(std::string_view /*field*/) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<SortedSetDocValues>>
    get_sorted_set_doc_values(std::string_view /*field*/) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norm_values(std::string_view /*field*/) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<DocValuesSkipper>> get_doc_values_skipper(
            std::string_view /*field*/) override {
        return nullptr;
    }

    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return &field_infos_; }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return static_cast<const Bits *>(nullptr); }

private:
    std::string field_;
    std::map<std::string, std::vector<int32_t>> terms_;
    int32_t doc_count_;
    int32_t max_doc_;
    FieldInfos field_infos_{FieldInfos::none()};
};

/// A similarity that ignores the statistics, so only the scorer type matters.
class OneSimScorer final : public SimScorer {
public:
    [[nodiscard]] float score(float freq, int64_t norm) const override {
        return freq + static_cast<float>(norm);
    }
};

class OneSimilarity final : public Similarity {
public:
    [[nodiscard]] Result<std::unique_ptr<SimScorer>> scorer(
            float /*boost*/, const CollectionStatistics & /*collection_stats*/,
            std::span<const TermStatistics> /*term_stats*/) const override {
        return std::unique_ptr<SimScorer>(new OneSimScorer());
    }

    [[nodiscard]] std::string name() const override { return "OneSimilarity"; }
};

IndexSearcher make_searcher(std::map<std::string, std::vector<int32_t>> terms, int32_t doc_count, int32_t max_doc) {
    std::shared_ptr<IndexReader> reader = std::make_shared<TermsLeafReader>(std::string(kField), std::move(terms),
                                                                           doc_count, max_doc);
    return IndexSearcher(std::move(reader), std::make_shared<OneSimilarity>());
}

/// Drains the scorer of `query` over the searcher's only leaf.
std::vector<std::pair<int32_t, float>> collect(IndexSearcher &searcher, const Query &query, ScoreMode mode,
                                               float boost) {
    std::vector<std::pair<int32_t, float>> hits;
    auto weight = query.create_weight(searcher, mode, boost);
    EXPECT_TRUE(weight.has_value()) << (weight.has_value() ? "" : weight.error().to_string());
    if (!weight.has_value()) {
        return hits;
    }
    auto leaves = searcher.leaves();
    EXPECT_TRUE(leaves.has_value());
    if (!leaves.has_value()) {
        return hits;
    }
    auto scorer = weight.value()->scorer(leaves.value().front(), searcher);
    EXPECT_TRUE(scorer.has_value()) << (scorer.has_value() ? "" : scorer.error().to_string());
    if (!scorer.has_value() || scorer.value() == nullptr) {
        return hits;
    }
    while (true) {
        auto doc = scorer.value()->next_doc();
        EXPECT_TRUE(doc.has_value()) << (doc.has_value() ? "" : doc.error().to_string());
        if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
            break;
        }
        auto score = scorer.value()->score();
        EXPECT_TRUE(score.has_value());
        hits.emplace_back(doc.value(), score.has_value() ? score.value() : 0.0F);
    }
    return hits;
}

std::vector<int32_t> docs_of(const std::vector<std::pair<int32_t, float>> &hits) {
    std::vector<int32_t> docs;
    docs.reserve(hits.size());
    for (const auto &[doc, score] : hits) {
        (void)score;
        docs.push_back(doc);
    }
    return docs;
}

/// The dictionary of most tests: two terms, one shared document.
std::map<std::string, std::vector<int32_t>> small_dictionary() {
    return {{"foo", {0, 2}}, {"food", {1}}};
}

} // namespace

TEST(MultiTermRewriteTest, DefaultRewriteWrapsTheMultiTermQuery) {
    auto query = std::make_shared<PrefixQuery>(std::string(kField), "foo");
    IndexSearcher searcher = make_searcher(small_dictionary(), 3, 3);
    auto rewritten = query->rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value()) << rewritten.error().to_string();
    ASSERT_NE(rewritten.value(), nullptr);
    const auto *wrapper = dynamic_cast<const MultiTermQueryConstantScoreWrapper *>(rewritten.value().get());
    ASSERT_NE(wrapper, nullptr);
    EXPECT_EQ(wrapper->query()->field(), kField);
    EXPECT_EQ(wrapper->to_string("").value(), query->to_string("").value());
}

TEST(MultiTermRewriteTest, WrapperScoresEveryMatchWithTheBoost) {
    auto query = std::make_shared<PrefixQuery>(std::string(kField), "foo");
    IndexSearcher searcher = make_searcher(small_dictionary(), 3, 3);
    auto rewritten = query->rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value());
    auto hits = collect(searcher, *rewritten.value(), ScoreMode::TopScores, 2.5F);
    EXPECT_EQ(docs_of(hits), (std::vector<int32_t>{0, 1, 2}));
    for (const auto &[doc, score] : hits) {
        (void)doc;
        EXPECT_FLOAT_EQ(score, 2.5F);
    }
}

TEST(MultiTermRewriteTest, WrapperUnionsThePostingsBeyondTheBooleanThreshold) {
    std::map<std::string, std::vector<int32_t>> terms;
    for (int32_t i = 0; i < 20; ++i) {
        terms[std::format("t{:02}", i)] = {i};
    }
    auto query = std::make_shared<PrefixQuery>(std::string(kField), "t");
    IndexSearcher searcher = make_searcher(std::move(terms), 20, 20);
    auto rewritten = query->rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value());
    auto hits = collect(searcher, *rewritten.value(), ScoreMode::TopScores, 1.0F);
    ASSERT_EQ(hits.size(), 20U);
    for (size_t i = 0; i < hits.size(); ++i) {
        EXPECT_EQ(hits[i].first, static_cast<int32_t>(i));
        EXPECT_FLOAT_EQ(hits[i].second, 1.0F);
    }
}

TEST(MultiTermRewriteTest, WrapperUsesTheDenseTermShortcut) {
    // "all" occurs in every document that has a value for the field, so the other
    // term must not add anything.
    std::map<std::string, std::vector<int32_t>> terms;
    terms["all"] = {0, 1, 2, 3};
    terms["x"] = {0};
    auto query = std::make_shared<PrefixQuery>(std::string(kField), "");
    IndexSearcher searcher = make_searcher(std::move(terms), 4, 4);
    auto rewritten = query->rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value());
    auto hits = collect(searcher, *rewritten.value(), ScoreMode::TopScores, 3.0F);
    EXPECT_EQ(docs_of(hits), (std::vector<int32_t>{0, 1, 2, 3}));
    for (const auto &[doc, score] : hits) {
        (void)doc;
        EXPECT_FLOAT_EQ(score, 3.0F);
    }
}

TEST(MultiTermRewriteTest, WrapperOnAnUnknownFieldMatchesNothing) {
    auto query = std::make_shared<PrefixQuery>("title", "foo");
    IndexSearcher searcher = make_searcher(small_dictionary(), 3, 3);
    auto rewritten = query->rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value());
    EXPECT_TRUE(collect(searcher, *rewritten.value(), ScoreMode::TopScores, 1.0F).empty());
}

TEST(MultiTermRewriteTest, ScoringBooleanRewriteReturnsABooleanQuery) {
    auto query = std::make_shared<PrefixQuery>(std::string(kField), "foo",
                                               MultiTermRewriteMethod::kScoringBoolean);
    IndexSearcher searcher = make_searcher(small_dictionary(), 3, 3);
    auto rewritten = query->rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value());
    ASSERT_NE(rewritten.value(), nullptr);
    EXPECT_EQ(dynamic_cast<const ConstantScoreQuery *>(rewritten.value().get()), nullptr);
    const auto *boolean = dynamic_cast<const BooleanQuery *>(rewritten.value().get());
    ASSERT_NE(boolean, nullptr);
    EXPECT_EQ(boolean->clauses().size(), 2U);
    EXPECT_EQ(docs_of(collect(searcher, *rewritten.value(), ScoreMode::Complete, 1.0F)),
              (std::vector<int32_t>{0, 1, 2}));
}

TEST(MultiTermRewriteTest, TopTermsScoringBooleanKeepsTheRequestedSize) {
    std::map<std::string, std::vector<int32_t>> terms;
    for (int32_t i = 0; i < 6; ++i) {
        terms[std::format("t{}", i)] = {i};
    }
    auto query = std::make_shared<PrefixQuery>(
            std::string(kField), "t",
            MultiTermRewriteMethod(MultiTermRewriteMethod::Kind::kTopTermsScoringBoolean, 3));
    IndexSearcher searcher = make_searcher(std::move(terms), 6, 6);
    auto rewritten = query->rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value()) << rewritten.error().to_string();
    const auto *boolean = dynamic_cast<const BooleanQuery *>(rewritten.value().get());
    ASSERT_NE(boolean, nullptr);
    EXPECT_EQ(boolean->clauses().size(), 3U);
    // With every boost at 1.0 the lexicographically first terms win.
    std::vector<std::string> texts;
    for (const BooleanClause &clause : boolean->clauses()) {
        const auto *term_query = dynamic_cast<const TermQuery *>(clause.query.get());
        ASSERT_NE(term_query, nullptr);
        texts.push_back(term_query->term().text());
    }
    EXPECT_EQ(texts, (std::vector<std::string>{"t0", "t1", "t2"}));
}

TEST(MultiTermRewriteTest, TopTermsBoostOnlyWrapsClausesInConstantScore) {
    auto query = std::make_shared<PrefixQuery>(
            std::string(kField), "foo",
            MultiTermRewriteMethod(MultiTermRewriteMethod::Kind::kTopTermsBoostOnlyBoolean, 2));
    IndexSearcher searcher = make_searcher(small_dictionary(), 3, 3);
    auto rewritten = query->rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value()) << rewritten.error().to_string();
    const auto *boolean = dynamic_cast<const BooleanQuery *>(rewritten.value().get());
    ASSERT_NE(boolean, nullptr);
    ASSERT_EQ(boolean->clauses().size(), 2U);
    for (const BooleanClause &clause : boolean->clauses()) {
        EXPECT_NE(dynamic_cast<const ConstantScoreQuery *>(clause.query.get()), nullptr);
    }
}

TEST(MultiTermRewriteTest, TopTermsBlendedProducesABlendedTermQuery) {
    auto query = std::make_shared<PrefixQuery>(
            std::string(kField), "foo",
            MultiTermRewriteMethod(MultiTermRewriteMethod::Kind::kTopTermsBlendedFreqScoring, 2));
    IndexSearcher searcher = make_searcher(small_dictionary(), 3, 3);
    auto rewritten = query->rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value()) << rewritten.error().to_string();
    const auto *blended = dynamic_cast<const BlendedTermQuery *>(rewritten.value().get());
    ASSERT_NE(blended, nullptr);
    EXPECT_EQ(blended->terms().size(), 2U);

    // Blended queries are rewritten to a boolean query before scoring.
    auto final_query = searcher.rewrite(*blended);
    ASSERT_TRUE(final_query.has_value()) << final_query.error().to_string();
    const auto *boolean = dynamic_cast<const BooleanQuery *>(final_query.value().get());
    ASSERT_NE(boolean, nullptr);
    EXPECT_EQ(boolean->clauses().size(), 2U);
    EXPECT_EQ(docs_of(collect(searcher, *final_query.value(), ScoreMode::Complete, 1.0F)),
              (std::vector<int32_t>{0, 1, 2}));
}

TEST(MultiTermRewriteTest, BlendedTermQueryBlendsTheStatistics) {
    std::map<std::string, std::vector<int32_t>> terms;
    terms["foo"] = {0, 2};
    terms["food"] = {1};
    IndexSearcher searcher = make_searcher(std::move(terms), 3, 3);

    BlendedTermQuery::Builder builder;
    Term foo = Term::from_text(std::string(kField), "foo");
    Term food = Term::from_text(std::string(kField), "food");
    ASSERT_TRUE(builder.add(foo, 1.0F).has_value());
    ASSERT_TRUE(builder.add(food, 1.0F).has_value());
    auto blended = builder.build();
    ASSERT_TRUE(blended.has_value()) << blended.error().to_string();

    auto rewritten = blended.value().rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value()) << rewritten.error().to_string();
    const auto *boolean = dynamic_cast<const BooleanQuery *>(rewritten.value().get());
    ASSERT_NE(boolean, nullptr);
    ASSERT_EQ(boolean->clauses().size(), 2U);
    // The blended document frequency is the maximum, the blended total term
    // frequency the sum, so both terms score with the same statistics.
    for (const BooleanClause &clause : boolean->clauses()) {
        const auto *term_query = dynamic_cast<const TermQuery *>(clause.query.get());
        ASSERT_NE(term_query, nullptr);
        ASSERT_NE(term_query->term_states(), nullptr);
        EXPECT_EQ(term_query->term_states()->doc_freq().value(), 2);
        EXPECT_EQ(term_query->term_states()->total_term_freq().value(), 3);
    }
}

TEST(MultiTermRewriteTest, BlendedTermQueryCanUseTheDisjunctionMaxRewrite) {
    std::map<std::string, std::vector<int32_t>> terms;
    terms["foo"] = {0, 2};
    terms["bar"] = {1};
    IndexSearcher searcher = make_searcher(std::move(terms), 3, 3);

    BlendedTermQuery::Builder builder;
    builder.set_disjunction_max_rewrite(0.5F);
    ASSERT_TRUE(builder.add(Term::from_text(std::string(kField), "foo"), 1.0F).has_value());
    ASSERT_TRUE(builder.add(Term::from_text(std::string(kField), "bar"), 1.0F).has_value());
    auto blended = builder.build();
    ASSERT_TRUE(blended.has_value()) << blended.error().to_string();
    EXPECT_EQ(blended.value().rewrite_method(), BlendedTermQuery::RewriteMethod::kDisjunctionMax);
    EXPECT_FLOAT_EQ(blended.value().tie_breaker_multiplier(), 0.5F);

    auto rewritten = blended.value().rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value()) << rewritten.error().to_string();
    const auto *disjunction = dynamic_cast<const DisjunctionMaxQuery *>(rewritten.value().get());
    ASSERT_NE(disjunction, nullptr);
    EXPECT_EQ(disjunction->disjuncts().size(), 2U);
    EXPECT_FLOAT_EQ(disjunction->tie_breaker_multiplier(), 0.5F);

    // The blended statistics survive the disjunction: both disjuncts are term
    // queries over the effective document frequency.
    for (const auto &disjunct : disjunction->disjuncts()) {
        const auto *term_query = dynamic_cast<const TermQuery *>(disjunct.get());
        ASSERT_NE(term_query, nullptr);
        ASSERT_NE(term_query->term_states(), nullptr);
        EXPECT_EQ(term_query->term_states()->doc_freq().value(), 2);
    }
}

TEST(MultiTermRewriteTest, BlendedTermQueryEqualitySeesTheRewriteMethod) {
    BlendedTermQuery::Builder plain;
    ASSERT_TRUE(plain.add(Term::from_text(std::string(kField), "foo"), 1.0F).has_value());
    auto boolean = plain.build();
    ASSERT_TRUE(boolean.has_value());

    BlendedTermQuery::Builder dismax;
    dismax.set_disjunction_max_rewrite(0.01F);
    ASSERT_TRUE(dismax.add(Term::from_text(std::string(kField), "foo"), 1.0F).has_value());
    auto other = dismax.build();
    ASSERT_TRUE(other.has_value());

    EXPECT_FALSE(boolean.value().equals(other.value()));
    EXPECT_TRUE(boolean.value().equals(boolean.value()));
}

TEST(MultiTermRewriteTest, BlendedTermQueryNeedsATerm) {
    BlendedTermQuery::Builder builder;
    auto blended = builder.build();
    ASSERT_FALSE(blended.has_value());
    EXPECT_EQ(blended.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(MultiTermRewriteTest, RewriteMethodValuesCompareKindAndSize) {
    EXPECT_EQ(MultiTermRewriteMethod::kConstantScore, MultiTermRewriteMethod::kConstantScore);
    EXPECT_NE(MultiTermRewriteMethod::kConstantScore, MultiTermRewriteMethod::kScoringBoolean);
    const MultiTermRewriteMethod two(MultiTermRewriteMethod::Kind::kTopTermsScoringBoolean, 2);
    const MultiTermRewriteMethod three(MultiTermRewriteMethod::Kind::kTopTermsScoringBoolean, 3);
    EXPECT_NE(two, three);
    EXPECT_EQ(two.kind(), MultiTermRewriteMethod::Kind::kTopTermsScoringBoolean);
    EXPECT_EQ(three.size(), 3U);
}

TEST(MultiTermRewriteTest, QueryEqualitySeesTheRewriteMethod) {
    auto constant = std::make_shared<PrefixQuery>(std::string(kField), "foo");
    auto scoring = std::make_shared<PrefixQuery>(std::string(kField), "foo",
                                                 MultiTermRewriteMethod::kScoringBoolean);
    EXPECT_FALSE(constant->equals(*scoring));
    auto top_two = std::make_shared<PrefixQuery>(
            std::string(kField), "foo",
            MultiTermRewriteMethod(MultiTermRewriteMethod::Kind::kTopTermsScoringBoolean, 2));
    auto top_three = std::make_shared<PrefixQuery>(
            std::string(kField), "foo",
            MultiTermRewriteMethod(MultiTermRewriteMethod::Kind::kTopTermsScoringBoolean, 3));
    EXPECT_FALSE(top_two->equals(*top_three));
    auto same = std::make_shared<PrefixQuery>(
            std::string(kField), "foo",
            MultiTermRewriteMethod(MultiTermRewriteMethod::Kind::kTopTermsScoringBoolean, 3));
    EXPECT_TRUE(top_three->equals(*same));
    EXPECT_EQ(top_three->hash(), same->hash());
}

TEST(MultiTermRewriteTest, QueryWithoutSharedOwnershipExpandsInsteadOfWrapping) {
    // A stack-allocated query cannot hand out a shared pointer, so the rewrite
    // falls back to the plain constant score expansion.
    const PrefixQuery query(std::string(kField), "foo");
    IndexSearcher searcher = make_searcher(small_dictionary(), 3, 3);
    auto rewritten = query.rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value()) << rewritten.error().to_string();
    ASSERT_NE(rewritten.value(), nullptr);
    const auto *constant_score = dynamic_cast<const ConstantScoreQuery *>(rewritten.value().get());
    ASSERT_NE(constant_score, nullptr);
    EXPECT_NE(dynamic_cast<const BooleanQuery *>(constant_score->query().get()), nullptr);
}
