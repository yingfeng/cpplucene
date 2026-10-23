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
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.phrase_query;

import lucene_core;

using namespace lucene;

namespace {

/// The positions of one term in one document.
struct TermDoc {
    int32_t doc{0};
    std::vector<int32_t> positions;
};

/// The postings of one term, with positions.
class PositionPostings final : public PostingsEnum {
public:
    explicit PositionPostings(std::vector<TermDoc> docs) : docs_(std::move(docs)) {}

    [[nodiscard]] int32_t doc_id() const override {
        if (!positioned_) {
            return -1;
        }
        return index_ < docs_.size() ? docs_[index_].doc : NO_MORE_DOCS;
    }

    [[nodiscard]] Result<int32_t> next_doc() override {
        if (positioned_ && index_ < docs_.size()) {
            ++index_;
        }
        positioned_ = true;
        position_ = -1;
        return doc_id();
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        if (!positioned_) {
            auto first = next_doc();
            if (!first.has_value()) {
                return std::unexpected(std::move(first.error()));
            }
        }
        while (doc_id() != NO_MORE_DOCS && doc_id() < target) {
            auto next = next_doc();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
        }
        return doc_id();
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(docs_.size()); }

    [[nodiscard]] Result<int32_t> freq() override {
        if (doc_id() < 0 || doc_id() == NO_MORE_DOCS) {
            return err(LuceneError::illegal_state("postings are not positioned"));
        }
        return static_cast<int32_t>(docs_[index_].positions.size());
    }

    [[nodiscard]] Result<int32_t> next_position() override {
        const std::vector<int32_t> &positions = docs_[index_].positions;
        ++position_;
        if (position_ >= static_cast<int32_t>(positions.size())) {
            return err(LuceneError::illegal_state("no more positions in this document"));
        }
        return positions[static_cast<size_t>(position_)];
    }

    [[nodiscard]] Result<int32_t> start_offset() const override { return -1; }
    [[nodiscard]] Result<int32_t> end_offset() const override { return -1; }
    [[nodiscard]] Result<std::optional<std::span<const uint8_t>>> get_payload() const override {
        return std::optional<std::span<const uint8_t>>{};
    }

private:
    std::vector<TermDoc> docs_;
    size_t index_{0};
    bool positioned_{false};
    int32_t position_{-1};
};

/// A [`TermsEnum`] over sorted `(term, postings)` pairs.
class MapTermsEnum final : public TermsEnum {
public:
    explicit MapTermsEnum(std::vector<std::pair<std::string, std::vector<TermDoc>>> terms)
            : terms_(std::move(terms)) {}

    [[nodiscard]] Result<const BytesRef *> next() override {
        if (!positioned_) {
            positioned_ = true;
            index_ = 0;
        } else {
            ++index_;
        }
        if (index_ >= terms_.size()) {
            return static_cast<const BytesRef *>(nullptr);
        }
        return &current();
    }

    [[nodiscard]] Result<const BytesRef *> term() const override {
        if (!positioned_ || index_ >= terms_.size()) {
            return err(LuceneError::illegal_state("terms enum is not positioned"));
        }
        return &current();
    }

    [[nodiscard]] Result<int64_t> ord() const override { return static_cast<int64_t>(index_); }

    [[nodiscard]] Result<int32_t> doc_freq() override {
        if (index_ >= terms_.size()) {
            return err(LuceneError::illegal_state("terms enum is not positioned"));
        }
        return static_cast<int32_t>(terms_[index_].second.size());
    }

    [[nodiscard]] Result<int64_t> total_term_freq() override {
        auto frequency = doc_freq();
        if (!frequency.has_value()) {
            return std::unexpected(std::move(frequency.error()));
        }
        return static_cast<int64_t>(frequency.value());
    }

    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>> postings_with_flags(
            std::unique_ptr<PostingsEnum> /*reuse*/, int32_t /*flags*/) override {
        if (index_ >= terms_.size()) {
            return err(LuceneError::illegal_state("terms enum is not positioned"));
        }
        return std::unique_ptr<PostingsEnum>(std::make_unique<PositionPostings>(terms_[index_].second));
    }

    [[nodiscard]] Result<std::unique_ptr<ImpactsEnum>> impacts(int32_t /*flags*/) override {
        return err(LuceneError::unsupported_operation("MapTermsEnum has no impacts"));
    }

    [[nodiscard]] Result<AttributeSource *> attributes() override {
        return err(LuceneError::unsupported_operation("MapTermsEnum has no attributes"));
    }
    [[nodiscard]] Result<AttributeSource *> attributes_mut() override {
        return err(LuceneError::unsupported_operation("MapTermsEnum has no attributes"));
    }

    [[nodiscard]] Result<SeekStatus> seek_ceil(std::span<const uint8_t> target) override {
        const std::string text(reinterpret_cast<const char *>(target.data()), target.size());
        positioned_ = true;
        for (size_t i = 0; i < terms_.size(); ++i) {
            if (terms_[i].first < text) {
                continue;
            }
            index_ = i;
            return terms_[i].first == text ? SeekStatus::Found : SeekStatus::NotFound;
        }
        index_ = terms_.size();
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
        positioned_ = true;
        index_ = static_cast<size_t>(ord);
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

    [[nodiscard]] Result<TermStateEnum> term_state() override {
        return TermStateEnum(OrdTermState(static_cast<int64_t>(index_)));
    }

private:
    [[nodiscard]] const BytesRef &current() const {
        cache_ = BytesRef::from_string(terms_[index_].first);
        return cache_;
    }

    std::vector<std::pair<std::string, std::vector<TermDoc>>> terms_;
    size_t index_{0};
    bool positioned_{false};
    mutable BytesRef cache_{};
};

/// [`Terms`] over sorted `(term, postings)` pairs of one field.
class MapTerms final : public Terms {
public:
    MapTerms(std::vector<std::pair<std::string, std::vector<TermDoc>>> terms, int32_t doc_count)
            : terms_(std::move(terms)), doc_count_(doc_count) {}

    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> iterator() const override {
        return std::unique_ptr<TermsEnum>(std::make_unique<MapTermsEnum>(terms_));
    }

    [[nodiscard]] Result<int64_t> size() const override { return static_cast<int64_t>(terms_.size()); }

    [[nodiscard]] Result<int64_t> sum_total_term_freq() const override {
        int64_t total = 0;
        for (const auto &[term, docs] : terms_) {
            (void)term;
            for (const TermDoc &doc : docs) {
                total += static_cast<int64_t>(doc.positions.size());
            }
        }
        return total;
    }

    [[nodiscard]] Result<int64_t> sum_doc_freq() const override {
        int64_t total = 0;
        for (const auto &[term, docs] : terms_) {
            (void)term;
            total += static_cast<int64_t>(docs.size());
        }
        return total;
    }

    [[nodiscard]] Result<int32_t> doc_count() const override { return doc_count_; }
    [[nodiscard]] bool has_freqs() const override { return true; }
    [[nodiscard]] bool has_offsets() const override { return false; }
    [[nodiscard]] bool has_positions() const override { return true; }
    [[nodiscard]] bool has_payloads() const override { return false; }

private:
    std::vector<std::pair<std::string, std::vector<TermDoc>>> terms_;
    int32_t doc_count_{0};
};

/// A [`LeafReader`] over one positions field; norms are absent, so the norm `1`
/// is used.
class PositionsLeafReader final : public LeafReader {
public:
    PositionsLeafReader(std::vector<std::pair<std::string, std::vector<TermDoc>>> terms, int32_t max_doc)
            : terms_(std::move(terms)), max_doc_(max_doc) {}

    [[nodiscard]] Result<int32_t> max_doc() const override { return max_doc_; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return max_doc_; }

    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override { return nullptr; }
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override { return nullptr; }

    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view field) override {
        if (field != kField) {
            return std::unique_ptr<Terms>{};
        }
        return std::unique_ptr<Terms>(std::make_unique<MapTerms>(terms_, max_doc_));
    }

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric_doc_values(
            std::string_view /*field*/) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> get_binary_doc_values(
            std::string_view /*field*/) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted_doc_values(
            std::string_view /*field*/) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>> get_sorted_numeric_doc_values(
            std::string_view /*field*/) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<SortedSetDocValues>> get_sorted_set_doc_values(
            std::string_view /*field*/) override {
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

    static constexpr std::string_view kField = "body";

private:
    std::vector<std::pair<std::string, std::vector<TermDoc>>> terms_;
    int32_t max_doc_{0};
    FieldInfos field_infos_{FieldInfos::none()};
};

/// The corpus:
///  0: "the quick brown fox"      the@0 quick@1 brown@2 fox@3
///  1: "the quick fox brown"      the@0 quick@1 fox@2 brown@3
///  2: "quick brown fox"          quick@0 brown@1 fox@2
///  3: "the quick very brown fox" the@0 quick@1 very@2 brown@3 fox@4
std::vector<std::pair<std::string, std::vector<TermDoc>>> corpus() {
    return {
            {"brown", {{0, {2}}, {1, {3}}, {2, {1}}, {3, {3}}}},
            {"fox", {{0, {3}}, {1, {2}}, {2, {2}}, {3, {4}}}},
            {"quick", {{0, {1}}, {1, {1}}, {2, {0}}, {3, {1}}}},
            {"the", {{0, {0}}, {1, {0}}, {3, {0}}}},
            {"very", {{3, {2}}}},
    };
}

IndexSearcher make_searcher() {
    std::shared_ptr<IndexReader> reader = std::make_shared<PositionsLeafReader>(corpus(), 4);
    return IndexSearcher(std::move(reader));
}

Term term(std::string text) { return Term::from_text(std::string(PositionsLeafReader::kField), text); }

/// The documents the scorer of `query` reports, in doc order.
std::vector<int32_t> matching_docs(const Query &query, const IndexSearcher &searcher) {
    std::vector<int32_t> docs;
    auto weight = query.create_weight(searcher, ScoreMode::Complete, 1.0F);
    EXPECT_TRUE(weight.has_value()) << weight.error().to_string();
    if (!weight.has_value()) {
        return docs;
    }
    auto leaves = searcher.leaves();
    EXPECT_TRUE(leaves.has_value()) << leaves.error().to_string();
    if (!leaves.has_value()) {
        return docs;
    }
    for (const LeafReaderContext &context : leaves.value()) {
        auto scorer = weight.value()->scorer(context, searcher);
        EXPECT_TRUE(scorer.has_value()) << scorer.error().to_string();
        if (!scorer.has_value() || scorer.value() == nullptr) {
            continue;
        }
        auto doc = scorer.value()->next_doc();
        while (doc.has_value() && doc.value() != NO_MORE_DOCS) {
            docs.push_back(doc.value());
            doc = scorer.value()->next_doc();
        }
    }
    return docs;
}

// ---------------------------------------------------------------------------
// construction and to_string
// ---------------------------------------------------------------------------

TEST(PhraseQueryTest, ToStringPrintsTheTerms) {
    auto query = PhraseQuery::create("body", {term("quick"), term("brown"), term("fox")});
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(query.value().to_string(""), "body:\"quick brown fox\"");
    EXPECT_EQ(query.value().to_string("body"), "\"quick brown fox\"");
    EXPECT_EQ(query.value().to_string("title"), "body:\"quick brown fox\"");
}

TEST(PhraseQueryTest, ToStringPrintsTheSlop) {
    auto query = PhraseQuery::with_slop("body", {term("quick"), term("brown")}, 2);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(query.value().to_string("body"), "\"quick brown\"~2");
}

TEST(PhraseQueryTest, ToStringPrintsGapsAndSharedPositions) {
    auto gap = PhraseQuery::with_positions("body", {term("quick"), term("brown")}, {0, 2}, 0);
    ASSERT_TRUE(gap.has_value()) << gap.error().to_string();
    EXPECT_EQ(gap.value().to_string("body"), "\"quick ? brown\"");

    auto shared = PhraseQuery::with_positions("body", {term("quick"), term("fast")}, {0, 0}, 0);
    ASSERT_TRUE(shared.has_value()) << shared.error().to_string();
    EXPECT_EQ(shared.value().to_string("body"), "\"quick|fast\"");
}

TEST(PhraseQueryTest, RejectsAnEmptyPhrase) {
    auto query = PhraseQuery::create("body", {});
    ASSERT_FALSE(query.has_value());
    EXPECT_EQ(query.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(PhraseQueryTest, RejectsANegativeSlop) {
    auto query = PhraseQuery::with_slop("body", {term("quick")}, -1);
    ASSERT_FALSE(query.has_value());
    EXPECT_EQ(query.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(PhraseQueryTest, RejectsMismatchedPositions) {
    auto query = PhraseQuery::with_positions("body", {term("quick"), term("brown")}, {0}, 0);
    ASSERT_FALSE(query.has_value());
    EXPECT_EQ(query.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(PhraseQueryTest, RejectsPositionsOutOfOrder) {
    auto query = PhraseQuery::with_positions("body", {term("quick"), term("brown")}, {2, 1}, 0);
    ASSERT_FALSE(query.has_value());
    EXPECT_EQ(query.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(PhraseQueryTest, RejectsTermsOnOtherFields) {
    std::vector<Term> terms{term("quick"), Term::from_text("title", "brown")};
    auto query = PhraseQuery::create("body", std::move(terms));
    ASSERT_FALSE(query.has_value());
    EXPECT_EQ(query.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(PhraseQueryTest, TermTextsArePrefixedWithTheField) {
    auto query = PhraseQuery::from_term_texts("body", {"quick", "brown"});
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(query.value().terms().size(), 2);
    EXPECT_EQ(query.value().positions(), (std::vector<int32_t>{0, 1}));
    EXPECT_EQ(query.value().slop(), 0);
}

TEST(PhraseQueryTest, EqualityComparesTermsPositionsAndSlop) {
    auto left = PhraseQuery::with_slop("body", {term("quick"), term("brown")}, 1);
    auto same = PhraseQuery::with_slop("body", {term("quick"), term("brown")}, 1);
    auto other_slop = PhraseQuery::with_slop("body", {term("quick"), term("brown")}, 2);
    auto other_position = PhraseQuery::with_positions("body", {term("quick"), term("brown")}, {0, 2}, 1);
    ASSERT_TRUE(left.has_value());
    ASSERT_TRUE(same.has_value());
    ASSERT_TRUE(other_slop.has_value());
    ASSERT_TRUE(other_position.has_value());
    EXPECT_TRUE(left.value().equals(same.value()));
    EXPECT_EQ(left.value().hash(), same.value().hash());
    EXPECT_FALSE(left.value().equals(other_slop.value()));
    EXPECT_FALSE(left.value().equals(other_position.value()));
}

TEST(PhraseQueryTest, ASingleTermPhraseRewritesToATermQuery) {
    auto query = PhraseQuery::create("body", {term("quick")});
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    const IndexSearcher searcher = make_searcher();
    auto rewritten = query.value().rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value());
    ASSERT_NE(rewritten.value(), nullptr);
    EXPECT_NE(dynamic_cast<const TermQuery *>(rewritten.value().get()), nullptr);
}

TEST(PhraseQueryTest, AMultiTermPhraseDoesNotRewrite) {
    auto query = PhraseQuery::create("body", {term("quick"), term("brown")});
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    const IndexSearcher searcher = make_searcher();
    auto rewritten = query.value().rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value());
    EXPECT_EQ(rewritten.value(), nullptr);
}

// ---------------------------------------------------------------------------
// matching
// ---------------------------------------------------------------------------

TEST(PhraseQueryTest, ExactPhraseMatchesOnlyAdjacentPositions) {
    auto query = PhraseQuery::create("body", {term("quick"), term("brown")});
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    const IndexSearcher searcher = make_searcher();
    EXPECT_EQ(matching_docs(query.value(), searcher), (std::vector<int32_t>{0, 2}));
}

TEST(PhraseQueryTest, ExactPhraseNeedsEveryTerm) {
    auto query = PhraseQuery::create("body", {term("quick"), term("absent")});
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    const IndexSearcher searcher = make_searcher();
    EXPECT_TRUE(matching_docs(query.value(), searcher).empty());
}

TEST(PhraseQueryTest, ExactPhraseOfThreeTerms) {
    auto query = PhraseQuery::create("body", {term("quick"), term("brown"), term("fox")});
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    const IndexSearcher searcher = make_searcher();
    EXPECT_EQ(matching_docs(query.value(), searcher), (std::vector<int32_t>{0, 2}));
}

TEST(PhraseQueryTest, SlopOneMatchesOnePositionOfMovement) {
    auto query = PhraseQuery::with_slop("body", {term("quick"), term("brown")}, 1);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    const IndexSearcher searcher = make_searcher();
    // Document 1 has "quick fox brown" and document 3 "quick very brown": both
    // have one position between the terms.
    EXPECT_EQ(matching_docs(query.value(), searcher), (std::vector<int32_t>{0, 1, 2, 3}));
}

TEST(PhraseQueryTest, SlopZeroIsTheExactMatch) {
    auto query = PhraseQuery::with_slop("body", {term("quick"), term("brown")}, 0);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    const IndexSearcher searcher = make_searcher();
    EXPECT_EQ(matching_docs(query.value(), searcher), (std::vector<int32_t>{0, 2}));
}

TEST(PhraseQueryTest, PhraseWithAGapNeedsTheGap) {
    // "quick ? brown": brown has to sit two positions after quick.
    auto query = PhraseQuery::with_positions("body", {term("quick"), term("brown")}, {0, 2}, 0);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    const IndexSearcher searcher = make_searcher();
    EXPECT_EQ(matching_docs(query.value(), searcher), (std::vector<int32_t>{1, 3}));
}

TEST(PhraseQueryTest, RepeatedTermMustAppearTwice) {
    auto twice = PhraseQuery::create("body", {term("quick"), term("quick")});
    ASSERT_TRUE(twice.has_value()) << twice.error().to_string();
    const IndexSearcher searcher = make_searcher();
    // No document repeats the term, so even with slop nothing matches.
    EXPECT_TRUE(matching_docs(twice.value(), searcher).empty());

    auto loose = PhraseQuery::with_slop("body", {term("quick"), term("quick")}, 5);
    ASSERT_TRUE(loose.has_value()) << loose.error().to_string();
    EXPECT_TRUE(matching_docs(loose.value(), searcher).empty());
}

TEST(PhraseQueryTest, SearchReturnsTheMatchingDocuments) {
    auto query = PhraseQuery::create("body", {term("the"), term("quick")});
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    const IndexSearcher searcher = make_searcher();
    auto top = searcher.search(query.value(), 10);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    ASSERT_EQ(top.value().score_docs.size(), 3);
    EXPECT_EQ(top.value().total_hits.value, 3);
}

// ---------------------------------------------------------------------------
// explanation and visiting
// ---------------------------------------------------------------------------

TEST(PhraseQueryTest, ExplainMatchesAndMisses) {
    auto query = PhraseQuery::create("body", {term("quick"), term("brown")});
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    const IndexSearcher searcher = make_searcher();
    auto weight = query.value().create_weight(searcher, ScoreMode::Complete, 1.0F);
    ASSERT_TRUE(weight.has_value()) << weight.error().to_string();
    auto leaves = searcher.leaves();
    ASSERT_TRUE(leaves.has_value()) << leaves.error().to_string();
    ASSERT_EQ(leaves.value().size(), 1);

    auto matched = weight.value()->explain(leaves.value()[0], 0, searcher);
    ASSERT_TRUE(matched.has_value()) << matched.error().to_string();
    EXPECT_TRUE(matched.value().is_match());
    EXPECT_GT(number_as_f64(matched.value().value()), 0.0);

    auto missed = weight.value()->explain(leaves.value()[0], 1, searcher);
    ASSERT_TRUE(missed.has_value()) << missed.error().to_string();
    EXPECT_FALSE(missed.value().is_match());
}

TEST(PhraseQueryTest, VisitingCollectsTheTerms) {
    auto query = PhraseQuery::create("body", {term("quick"), term("brown")});
    ASSERT_TRUE(query.has_value()) << query.error().to_string();

    class CollectingVisitor final : public QueryVisitor {
    public:
        [[nodiscard]] VoidResult consume_terms(const Query & /*query*/, std::span<const Term> terms) override {
            for (const Term &one : terms) {
                texts.push_back(one.text());
            }
            return {};
        }
        std::vector<std::string> texts;
    };

    CollectingVisitor visitor;
    auto visited = query.value().visit(visitor);
    ASSERT_TRUE(visited.has_value()) << visited.error().to_string();
    EXPECT_EQ(visitor.texts, (std::vector<std::string>{"quick", "brown"}));
}

// ---------------------------------------------------------------------------
// BoostQuery
// ---------------------------------------------------------------------------

TEST(PhraseQueryTest, BoostToStringPrintsOneDecimal) {
    auto inner = PhraseQuery::create("body", {term("quick"), term("brown")});
    ASSERT_TRUE(inner.has_value()) << inner.error().to_string();
    auto query = BoostQuery::create(std::make_shared<PhraseQuery>(inner.value()), 2.0F);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(query.value().to_string("body"), "(\"quick brown\")^2.0");
}

TEST(PhraseQueryTest, BoostRejectsInvalidFactors) {
    auto inner = PhraseQuery::create("body", {term("quick"), term("brown")});
    ASSERT_TRUE(inner.has_value()) << inner.error().to_string();
    auto shared = std::make_shared<PhraseQuery>(inner.value());

    EXPECT_FALSE(BoostQuery::create(shared, -1.0F).has_value());
    EXPECT_FALSE(BoostQuery::create(shared, std::numeric_limits<float>::quiet_NaN()).has_value());
    EXPECT_FALSE(BoostQuery::create(shared, std::numeric_limits<float>::infinity()).has_value());
    EXPECT_FALSE(BoostQuery::create(shared, -0.0F).has_value());
}

TEST(PhraseQueryTest, BoostEqualityComparesTheFactor) {
    auto inner = PhraseQuery::create("body", {term("quick"), term("brown")});
    ASSERT_TRUE(inner.has_value()) << inner.error().to_string();
    auto shared = std::make_shared<PhraseQuery>(inner.value());
    auto left = BoostQuery::create(shared, 2.0F);
    auto same = BoostQuery::create(shared, 2.0F);
    auto other = BoostQuery::create(shared, 3.0F);
    ASSERT_TRUE(left.has_value());
    ASSERT_TRUE(same.has_value());
    ASSERT_TRUE(other.has_value());
    EXPECT_TRUE(left.value().equals(same.value()));
    EXPECT_EQ(left.value().hash(), same.value().hash());
    EXPECT_FALSE(left.value().equals(other.value()));
}

TEST(PhraseQueryTest, BoostOfOneUnwrapsItself) {
    auto inner = PhraseQuery::create("body", {term("quick"), term("brown")});
    ASSERT_TRUE(inner.has_value()) << inner.error().to_string();
    auto query = BoostQuery::create(std::make_shared<PhraseQuery>(inner.value()), 1.0F);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    const IndexSearcher searcher = make_searcher();
    auto rewritten = query.value().rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value()) << rewritten.error().to_string();
    ASSERT_NE(rewritten.value(), nullptr);
    EXPECT_TRUE(rewritten.value()->equals(inner.value()));
}

TEST(PhraseQueryTest, NestedBoostsAreFolded) {
    auto inner = PhraseQuery::create("body", {term("quick"), term("brown")});
    ASSERT_TRUE(inner.has_value()) << inner.error().to_string();
    auto first = BoostQuery::create(std::make_shared<PhraseQuery>(inner.value()), 2.0F);
    ASSERT_TRUE(first.has_value());
    auto second = BoostQuery::create(std::make_shared<BoostQuery>(first.value()), 3.0F);
    ASSERT_TRUE(second.has_value());

    const IndexSearcher searcher = make_searcher();
    auto rewritten = second.value().rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value()) << rewritten.error().to_string();
    ASSERT_NE(rewritten.value(), nullptr);
    const auto *folded = dynamic_cast<const BoostQuery *>(rewritten.value().get());
    ASSERT_NE(folded, nullptr);
    EXPECT_FLOAT_EQ(folded->boost(), 6.0F);
    EXPECT_TRUE(folded->query()->equals(inner.value()));
}

// ---------------------------------------------------------------------------
// MultiPhraseQuery
// ---------------------------------------------------------------------------

TEST(PhraseQueryTest, MultiPhraseMatchesOneTermPerPosition) {
    MultiPhraseQuery::Builder builder;
    ASSERT_TRUE(builder.add_terms({term("quick")}).has_value());
    ASSERT_TRUE(builder.add_terms({term("brown"), term("very")}).has_value());
    ASSERT_TRUE(builder.add_terms({term("fox")}).has_value());
    auto query = builder.build();
    ASSERT_TRUE(query.has_value()) << query.error().to_string();

    const IndexSearcher searcher = make_searcher();
    // Document 1 has the alternatives out of order and document 3 has them too
    // far apart.
    EXPECT_EQ(matching_docs(query.value(), searcher), (std::vector<int32_t>{0, 2}));
}

TEST(PhraseQueryTest, MultiPhraseWithSlopMatchesTheGap) {
    MultiPhraseQuery::Builder builder;
    ASSERT_TRUE(builder.set_slop(1).has_value());
    ASSERT_TRUE(builder.add_terms({term("quick")}).has_value());
    ASSERT_TRUE(builder.add_terms({term("brown"), term("very")}).has_value());
    ASSERT_TRUE(builder.add_terms({term("fox")}).has_value());
    auto query = builder.build();
    ASSERT_TRUE(query.has_value()) << query.error().to_string();

    const IndexSearcher searcher = make_searcher();
    EXPECT_EQ(matching_docs(query.value(), searcher), (std::vector<int32_t>{0, 2, 3}));
}

TEST(PhraseQueryTest, MultiPhraseToStringPrintsTheAlternatives) {
    MultiPhraseQuery::Builder builder;
    ASSERT_TRUE(builder.add_terms({term("quick")}).has_value());
    ASSERT_TRUE(builder.add_terms({term("brown"), term("very")}).has_value());
    ASSERT_TRUE(builder.add_terms_with_position({term("fox")}, 3).has_value());
    auto query = builder.build();
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(query.value().to_string(""), "body:\"quick (brown very) ? fox\"");
    EXPECT_EQ(query.value().to_string("body"), "\"quick (brown very) ? fox\"");
}

TEST(PhraseQueryTest, MultiPhraseRejectsBadPositionsAndFields) {
    MultiPhraseQuery::Builder empty_terms;
    EXPECT_FALSE(empty_terms.add_terms({}).has_value());

    MultiPhraseQuery::Builder negative_slop;
    EXPECT_FALSE(negative_slop.set_slop(-1).has_value());

    MultiPhraseQuery::Builder out_of_order;
    ASSERT_TRUE(out_of_order.add_terms_with_position({term("brown")}, 2).has_value());
    EXPECT_FALSE(out_of_order.add_terms_with_position({term("quick")}, 1).has_value());

    MultiPhraseQuery::Builder mixed;
    EXPECT_FALSE(mixed.add_terms({term("quick"), Term::from_text("title", "brown")}).has_value());
}

TEST(PhraseQueryTest, MultiPhraseEqualityComparesPositionsAndSlop) {
    auto build = [](int32_t slop) {
        MultiPhraseQuery::Builder builder;
        (void)builder.set_slop(slop);
        (void)builder.add_terms({term("quick")});
        (void)builder.add_terms({term("brown"), term("very")});
        return builder.build();
    };
    auto left = build(1);
    auto same = build(1);
    auto other = build(2);
    ASSERT_TRUE(left.has_value());
    ASSERT_TRUE(same.has_value());
    ASSERT_TRUE(other.has_value());
    EXPECT_TRUE(left.value().equals(same.value()));
    EXPECT_EQ(left.value().hash(), same.value().hash());
    EXPECT_FALSE(left.value().equals(other.value()));
}

TEST(PhraseQueryTest, SinglePositionMultiPhraseRewritesToADisjunction) {
    MultiPhraseQuery::Builder builder;
    ASSERT_TRUE(builder.add_terms({term("quick"), term("brown")}).has_value());
    auto query = builder.build();
    ASSERT_TRUE(query.has_value()) << query.error().to_string();

    const IndexSearcher searcher = make_searcher();
    auto rewritten = query.value().rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value()) << rewritten.error().to_string();
    ASSERT_NE(rewritten.value(), nullptr);
    const auto *boolean = dynamic_cast<const BooleanQuery *>(rewritten.value().get());
    ASSERT_NE(boolean, nullptr);
    EXPECT_EQ(boolean->clauses().size(), 2);
}

TEST(PhraseQueryTest, EmptyMultiPhraseRewritesToMatchNoDocs) {
    MultiPhraseQuery::Builder builder;
    auto query = builder.build();
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    const IndexSearcher searcher = make_searcher();
    auto rewritten = query.value().rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value()) << rewritten.error().to_string();
    ASSERT_NE(rewritten.value(), nullptr);
    EXPECT_NE(dynamic_cast<const MatchNoDocsQuery *>(rewritten.value().get()), nullptr);
}

TEST(PhraseQueryTest, MultiPhraseSearchAndExplain) {
    MultiPhraseQuery::Builder builder;
    ASSERT_TRUE(builder.add_terms({term("quick")}).has_value());
    ASSERT_TRUE(builder.add_terms({term("very")}).has_value());
    auto query = builder.build();
    ASSERT_TRUE(query.has_value()) << query.error().to_string();

    const IndexSearcher searcher = make_searcher();
    auto top = searcher.search(query.value(), 10);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    // Only document 3 has "the quick very brown fox".
    ASSERT_EQ(top.value().score_docs.size(), 1);
    EXPECT_EQ(top.value().score_docs.front().doc, 3);

    auto weight = query.value().create_weight(searcher, ScoreMode::Complete, 1.0F);
    ASSERT_TRUE(weight.has_value()) << weight.error().to_string();
    auto leaves = searcher.leaves();
    ASSERT_TRUE(leaves.has_value());
    auto explanation = weight.value()->explain(leaves.value()[0], 3, searcher);
    ASSERT_TRUE(explanation.has_value()) << explanation.error().to_string();
    EXPECT_TRUE(explanation.value().is_match());

    auto missed = weight.value()->explain(leaves.value()[0], 0, searcher);
    ASSERT_TRUE(missed.has_value()) << missed.error().to_string();
    EXPECT_FALSE(missed.value().is_match());
}

// ---------------------------------------------------------------------------
// NGramPhraseQuery
// ---------------------------------------------------------------------------

TEST(PhraseQueryTest, NGramPhraseDropsInteriorTerms) {
    auto phrase = PhraseQuery::create("body", {term("quick"), term("brown"), term("fox")});
    ASSERT_TRUE(phrase.has_value()) << phrase.error().to_string();
    const NGramPhraseQuery query(2, std::move(phrase.value()));
    EXPECT_EQ(query.n(), 2);

    const IndexSearcher searcher = make_searcher();
    auto rewritten = query.rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value()) << rewritten.error().to_string();
    ASSERT_NE(rewritten.value(), nullptr);
    const auto *optimized = dynamic_cast<const PhraseQuery *>(rewritten.value().get());
    ASSERT_NE(optimized, nullptr);
    EXPECT_EQ(optimized->terms().size(), 2);
    EXPECT_EQ(optimized->positions(), (std::vector<int32_t>{0, 2}));
    EXPECT_EQ(optimized->to_string("body"), "\"quick ? fox\"");
}

TEST(PhraseQueryTest, NGramPhraseLeavesShortPhrasesAlone) {
    auto phrase = PhraseQuery::create("body", {term("quick"), term("brown")});
    ASSERT_TRUE(phrase.has_value()) << phrase.error().to_string();
    const NGramPhraseQuery query(2, std::move(phrase.value()));

    const IndexSearcher searcher = make_searcher();
    auto rewritten = query.rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value()) << rewritten.error().to_string();
    // A two term phrase has nothing to drop, so it stays as it is.
    EXPECT_EQ(rewritten.value(), nullptr);
}

TEST(PhraseQueryTest, NGramPhraseWithSlopIsNotOptimized) {
    auto phrase = PhraseQuery::with_slop("body", {term("quick"), term("brown"), term("fox")}, 1);
    ASSERT_TRUE(phrase.has_value()) << phrase.error().to_string();
    const NGramPhraseQuery query(2, std::move(phrase.value()));

    const IndexSearcher searcher = make_searcher();
    auto rewritten = query.rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value()) << rewritten.error().to_string();
    EXPECT_EQ(rewritten.value(), nullptr);
}

TEST(PhraseQueryTest, NGramPhraseSearchUsesTheOptimizedQuery) {
    auto phrase = PhraseQuery::create("body", {term("quick"), term("brown"), term("fox")});
    ASSERT_TRUE(phrase.has_value()) << phrase.error().to_string();
    const NGramPhraseQuery query(2, std::move(phrase.value()));

    const IndexSearcher searcher = make_searcher();
    auto top = searcher.search(query, 10);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    ASSERT_EQ(top.value().score_docs.size(), 2);
    EXPECT_EQ(top.value().score_docs[0].doc, 0);
    EXPECT_EQ(top.value().score_docs[1].doc, 2);
}

TEST(PhraseQueryTest, NGramPhraseEqualityComparesTheSize) {
    auto phrase = PhraseQuery::create("body", {term("quick"), term("brown"), term("fox")});
    ASSERT_TRUE(phrase.has_value()) << phrase.error().to_string();
    const NGramPhraseQuery left(2, phrase.value());
    const NGramPhraseQuery same(2, phrase.value());
    const NGramPhraseQuery other(3, phrase.value());
    EXPECT_TRUE(left.equals(same));
    EXPECT_EQ(left.hash(), same.hash());
    EXPECT_FALSE(left.equals(other));
    EXPECT_EQ(left.to_string("body").value(), "\"quick brown fox\"");
}

} // namespace
