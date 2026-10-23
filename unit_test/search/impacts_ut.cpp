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
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.impacts;

import lucene_core;

using namespace lucene;

namespace {

constexpr int32_t kMaxDoc = 10;
constexpr int32_t kBlockShift = 2; // blocks of 4 documents
constexpr int32_t kBlockSize = 1 << kBlockShift;

/// `score(freq, norm) = freq * kFreqWeight + norm`, so a test can read the
/// maximum score of a block straight off its impacts.
constexpr float kFreqWeight = 10.0F;

class LinearSimScorer final : public SimScorer {
public:
    [[nodiscard]] float score(float freq, int64_t norm) const override {
        return freq * kFreqWeight + static_cast<float>(norm);
    }
};

/// Impacts built by hand: what the impacts of the current block are, and how far
/// they are valid.
class FakeImpacts final : public Impacts {
public:
    struct Level {
        int32_t upto;
        std::vector<Impact> impacts;
    };

    explicit FakeImpacts(std::vector<Level> levels) : levels_(std::move(levels)) {}

    [[nodiscard]] size_t num_levels() const override { return levels_.size(); }

    [[nodiscard]] int32_t get_doc_id_upto(size_t level) const override {
        if (level >= levels_.size()) {
            return NO_MORE_DOCS;
        }
        return levels_[level].upto;
    }

    [[nodiscard]] Result<std::vector<Impact>> get_impacts(size_t level) const override {
        if (level >= levels_.size()) {
            return err(LuceneError::illegal_argument("no such impacts level"));
        }
        return levels_[level].impacts;
    }

private:
    std::vector<Level> levels_;
};

/// An [`ImpactsEnum`] over documents `0 .. kMaxDoc - 1`, where every document
/// matches with frequency 1 and the impacts describe blocks of four documents.
class BlockImpactsEnum final : public ImpactsEnum {
public:
    BlockImpactsEnum() = default;

    // ---- DocIdSetIterator ----

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override { return advance(doc_ + 1); }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        doc_ = target < kMaxDoc ? target : NO_MORE_DOCS;
        return doc_;
    }

    [[nodiscard]] Result<int64_t> cost() const override { return kMaxDoc; }

    // ---- PostingsEnum ----

    /// Block `b` has frequency `b + 1`, which is what its impacts claim.
    [[nodiscard]] Result<int32_t> freq() override {
        if (doc_ < 0 || doc_ >= kMaxDoc) {
            return err(LuceneError::illegal_state("the postings are not positioned"));
        }
        return doc_ / kBlockSize + 1;
    }
    [[nodiscard]] Result<int32_t> next_position() override {
        return err(LuceneError::unsupported_operation("the fake postings have no positions"));
    }
    [[nodiscard]] Result<int32_t> start_offset() const override {
        return err(LuceneError::unsupported_operation("the fake postings have no offsets"));
    }
    [[nodiscard]] Result<int32_t> end_offset() const override {
        return err(LuceneError::unsupported_operation("the fake postings have no offsets"));
    }
    [[nodiscard]] Result<std::optional<std::span<const uint8_t>>> get_payload() const override {
        return std::nullopt;
    }

    // ---- ImpactsSource ----

    [[nodiscard]] VoidResult advance_shallow(int32_t target) override {
        // A target past the last document leaves nothing to shallow-advance
        // into, and must not overflow the block arithmetic.
        shallow_target_ = target > kMaxDoc - 1 ? kMaxDoc - 1 : target;
        return {};
    }

    [[nodiscard]] Result<std::unique_ptr<Impacts>> get_impacts() const override {
        const int32_t target = shallow_target_ < 0 ? 0 : shallow_target_;
        const int32_t block = target / kBlockSize;
        const int32_t block_upto = std::min((block + 1) * kBlockSize - 1, kMaxDoc - 1);
        // The impacts of a block grow with its index: block `b` has frequency
        // `b + 1`.
        std::vector<Impact> block_impacts{Impact{block + 1, 5}};
        std::vector<Impact> global_impacts{Impact{kMaxDoc / kBlockSize, 5}};
        std::vector<FakeImpacts::Level> levels;
        levels.push_back(FakeImpacts::Level{block_upto, std::move(block_impacts)});
        levels.push_back(FakeImpacts::Level{kMaxDoc - 1, std::move(global_impacts)});
        return std::unique_ptr<Impacts>(std::make_unique<FakeImpacts>(std::move(levels)));
    }

private:
    int32_t doc_{-1};
    int32_t shallow_target_{-1};
};

/// Drains a document iterator into the list of documents it visits.
std::vector<int32_t> drain(DocIdSetIterator &iterator) {
    std::vector<int32_t> docs;
    while (true) {
        auto doc = iterator.next_doc();
        EXPECT_TRUE(doc.has_value()) << (doc.has_value() ? "" : doc.error().to_string());
        if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
            break;
        }
        docs.push_back(doc.value());
    }
    return docs;
}

/// A terms dictionary with the single term "foo", whose postings are impacts
/// enums.  This is what makes a real search go through the pruning path.
class ImpactsTermsEnum final : public TermsEnum {
public:
    [[nodiscard]] Result<const BytesRef *> next() override {
        if (positioned_) {
            positioned_ = false;
            return static_cast<const BytesRef *>(nullptr);
        }
        positioned_ = true;
        return &term_;
    }

    [[nodiscard]] Result<SeekStatus> seek_ceil(std::span<const uint8_t> target) override {
        const std::string text(reinterpret_cast<const char *>(target.data()), target.size());
        if (text > kTerm) {
            positioned_ = false;
            return SeekStatus::End;
        }
        positioned_ = true;
        return text == kTerm ? SeekStatus::Found : SeekStatus::NotFound;
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
        if (ord != 0) {
            return err(LuceneError::illegal_argument("the dictionary has a single term"));
        }
        positioned_ = true;
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

    [[nodiscard]] Result<const BytesRef *> term() const override {
        if (!positioned_) {
            return err(LuceneError::illegal_state("the terms enum is not positioned"));
        }
        return &term_;
    }

    [[nodiscard]] Result<int64_t> ord() const override { return positioned_ ? 0 : -1; }

    [[nodiscard]] Result<int32_t> doc_freq() override { return kMaxDoc; }

    [[nodiscard]] Result<int64_t> total_term_freq() override {
        // Block `b` holds four documents of frequency `b + 1`.
        return 4 * 1 + 4 * 2 + 2 * 3;
    }

    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>> postings_with_flags(std::unique_ptr<PostingsEnum> /*reuse*/,
                                                                           int32_t /*flags*/) override {
        return std::unique_ptr<PostingsEnum>(std::make_unique<BlockImpactsEnum>());
    }

    [[nodiscard]] Result<std::unique_ptr<ImpactsEnum>> impacts(int32_t /*flags*/) override {
        return std::unique_ptr<ImpactsEnum>(std::make_unique<BlockImpactsEnum>());
    }

    [[nodiscard]] Result<AttributeSource *> attributes() override {
        return err(LuceneError::unsupported_operation("the fake terms have no attributes"));
    }
    [[nodiscard]] Result<AttributeSource *> attributes_mut() override {
        return err(LuceneError::unsupported_operation("the fake terms have no attributes"));
    }

    [[nodiscard]] Result<TermStateEnum> term_state() override {
        if (!positioned_) {
            return err(LuceneError::illegal_state("the terms enum is not positioned"));
        }
        return TermStateEnum(OrdTermState(0));
    }

private:
    static constexpr std::string_view kTerm = "foo";
    bool positioned_{false};
    BytesRef term_{BytesRef::from_string(std::string(kTerm))};
};

class ImpactsTerms final : public Terms {
public:
    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> iterator() const override {
        return std::unique_ptr<TermsEnum>(std::make_unique<ImpactsTermsEnum>());
    }
    [[nodiscard]] Result<int64_t> size() const override { return 1; }
    [[nodiscard]] Result<int64_t> sum_total_term_freq() const override { return 4 * 1 + 4 * 2 + 2 * 3; }
    [[nodiscard]] Result<int64_t> sum_doc_freq() const override { return kMaxDoc; }
    [[nodiscard]] Result<int32_t> doc_count() const override { return kMaxDoc; }
    [[nodiscard]] bool has_freqs() const override { return true; }
    [[nodiscard]] bool has_offsets() const override { return false; }
    [[nodiscard]] bool has_positions() const override { return false; }
    [[nodiscard]] bool has_payloads() const override { return false; }
};

/// Norms of `1` for every document.
class ConstantNorms final : public NumericDocValues {
public:
    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override { return advance(doc_ + 1); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        doc_ = target < kMaxDoc ? target : NO_MORE_DOCS;
        return doc_;
    }
    [[nodiscard]] Result<int64_t> cost() const override { return kMaxDoc; }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        auto advanced = advance(target);
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        return advanced.value() == target;
    }
    [[nodiscard]] Result<int64_t> long_value() override { return 1; }

private:
    int32_t doc_{-1};
};

class ImpactsLeafReader final : public LeafReader {
public:
    [[nodiscard]] Result<int32_t> max_doc() const override { return kMaxDoc; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return kMaxDoc; }

    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override {
        return std::unique_ptr<StoredFields>{};
    }
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override {
        return std::unique_ptr<TermVectors>{};
    }
    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view field) override {
        if (field != "body") {
            return std::unique_ptr<Terms>{};
        }
        return std::unique_ptr<Terms>(std::make_unique<ImpactsTerms>());
    }

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric_doc_values(
            std::string_view /*field*/) override {
        return std::unique_ptr<NumericDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> get_binary_doc_values(std::string_view /*field*/) override {
        return std::unique_ptr<BinaryDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted_doc_values(std::string_view /*field*/) override {
        return std::unique_ptr<SortedDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>>
    get_sorted_numeric_doc_values(std::string_view /*field*/) override {
        return std::unique_ptr<SortedNumericDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<SortedSetDocValues>>
    get_sorted_set_doc_values(std::string_view /*field*/) override {
        return std::unique_ptr<SortedSetDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norm_values(std::string_view field) override {
        if (field != "body") {
            return std::unique_ptr<NumericDocValues>{};
        }
        return std::unique_ptr<NumericDocValues>(std::make_unique<ConstantNorms>());
    }
    [[nodiscard]] Result<std::unique_ptr<DocValuesSkipper>> get_doc_values_skipper(
            std::string_view /*field*/) override {
        return std::unique_ptr<DocValuesSkipper>{};
    }

    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return &field_infos_; }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return static_cast<const Bits *>(nullptr); }

private:
    FieldInfos field_infos_{FieldInfos::none()};
};

class LinearSimilarity final : public Similarity {
public:
    [[nodiscard]] Result<std::unique_ptr<SimScorer>> scorer(
            float /*boost*/, const CollectionStatistics & /*collection_stats*/,
            std::span<const TermStatistics> /*term_stats*/) const override {
        return std::unique_ptr<SimScorer>(std::make_unique<LinearSimScorer>());
    }
    [[nodiscard]] std::string name() const override { return "LinearSimilarity"; }
};

} // namespace

TEST(ImpactsTest, MaxScoreCacheShallowAdvanceReportsTheBlockEnd) {
    BlockImpactsEnum source;
    MaxScoreCache cache(&source, std::make_shared<LinearSimScorer>());

    auto first = cache.advance_shallow(0);
    ASSERT_TRUE(first.has_value()) << first.error().to_string();
    EXPECT_EQ(first.value(), 3);

    auto second = cache.advance_shallow(5);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second.value(), 7);

    auto last = cache.advance_shallow(9);
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last.value(), 9);
}

TEST(ImpactsTest, MaxScoreCachePicksTheLevelThatCoversTheDocument) {
    BlockImpactsEnum source;
    MaxScoreCache cache(&source, std::make_shared<LinearSimScorer>());
    ASSERT_TRUE(cache.advance_shallow(0).has_value());

    // Level 0 covers documents 0..3 and reports frequency 1.
    auto block = cache.get_max_score(2);
    ASSERT_TRUE(block.has_value()) << block.error().to_string();
    EXPECT_FLOAT_EQ(block.value(), kFreqWeight + 5.0F);

    // Level 1 covers the whole postings list and reports frequency 2.
    auto whole = cache.get_max_score(9);
    ASSERT_TRUE(whole.has_value());
    EXPECT_FLOAT_EQ(whole.value(), 2 * kFreqWeight + 5.0F);

    // The global upper bound comes from the similarity scorer itself.
    EXPECT_FLOAT_EQ(cache.global_max_score(), std::numeric_limits<float>::max() * kFreqWeight + 1.0F);
}

TEST(ImpactsTest, MaxScoreCacheSkipsOnlyBlocksThatCannotBeCompetitive) {
    BlockImpactsEnum source;
    MaxScoreCache cache(&source, std::make_shared<LinearSimScorer>());
    ASSERT_TRUE(cache.advance_shallow(0).has_value());

    // The first block's maximum score is 15, so a threshold of 20 skips it.
    auto skip = cache.get_skip_up_to(20.0F);
    ASSERT_TRUE(skip.has_value()) << skip.error().to_string();
    EXPECT_EQ(skip.value(), 3);

    // A lower threshold keeps the block competitive.
    auto keep = cache.get_skip_up_to(10.0F);
    ASSERT_TRUE(keep.has_value());
    EXPECT_EQ(keep.value(), -1);

    // Above the global maximum every level is skippable: the coarsest level wins.
    auto everything = cache.get_skip_up_to(std::numeric_limits<float>::max());
    ASSERT_TRUE(everything.has_value());
    EXPECT_EQ(everything.value(), kMaxDoc - 1);
}

TEST(ImpactsTest, ImpactsDisiVisitsEveryDocumentWithoutAThreshold) {
    BlockImpactsEnum source;
    MaxScoreCache cache(&source, std::make_shared<LinearSimScorer>());
    auto disi = ImpactsDISI::from_source(&cache);
    ASSERT_TRUE(disi.has_value()) << disi.error().to_string();

    EXPECT_EQ(drain(*disi.value()), (std::vector<int32_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
}

TEST(ImpactsTest, ImpactsDisiSkipsBlocksBelowTheCompetitiveScore) {
    BlockImpactsEnum source;
    MaxScoreCache cache(&source, std::make_shared<LinearSimScorer>());
    auto disi = ImpactsDISI::from_source(&cache);
    ASSERT_TRUE(disi.has_value());

    // The first block tops out at 15, so a threshold of 20 skips documents 0..3.
    disi.value()->set_min_competitive_score(20.0F);
    EXPECT_EQ(drain(*disi.value()), (std::vector<int32_t>{4, 5, 6, 7, 8, 9}));
}

TEST(ImpactsTest, ImpactsDisiPrunesEverythingAboveTheGlobalMaximum) {
    BlockImpactsEnum source;
    MaxScoreCache cache(&source, std::make_shared<LinearSimScorer>());
    auto disi = ImpactsDISI::from_source(&cache);
    ASSERT_TRUE(disi.has_value());

    // No block can reach this threshold, so the whole postings list is skipped.
    disi.value()->set_min_competitive_score(std::numeric_limits<float>::max() - 1.0F);
    EXPECT_EQ(drain(*disi.value()), (std::vector<int32_t>{}));
}

TEST(ImpactsTest, ImpactsDisiForwardsTheCacheContracts) {
    BlockImpactsEnum source;
    MaxScoreCache cache(&source, std::make_shared<LinearSimScorer>());
    auto disi = ImpactsDISI::from_source(&cache);
    ASSERT_TRUE(disi.has_value());

    auto shallow = disi.value()->advance_shallow(4);
    ASSERT_TRUE(shallow.has_value()) << shallow.error().to_string();
    EXPECT_EQ(shallow.value(), 7);

    auto max_score = disi.value()->get_max_score(5);
    ASSERT_TRUE(max_score.has_value());
    EXPECT_FLOAT_EQ(max_score.value(), 2 * kFreqWeight + 5.0F);

    EXPECT_EQ(dynamic_cast<DocIdSetIterator *>(&disi.value()->max_score_cache().impacts_source()) != nullptr, true);
}

TEST(ImpactsTest, TermScorerUsesImpactsForBlockMaxScores) {
    auto scorer = TermScorer::from_impacts(std::make_unique<BlockImpactsEnum>(),
                                          std::make_shared<LinearSimScorer>(), nullptr, false);
    ASSERT_TRUE(scorer.has_value()) << scorer.error().to_string();
    EXPECT_TRUE(scorer.value()->has_impacts());

    auto shallow = scorer.value()->advance_shallow(0);
    ASSERT_TRUE(shallow.has_value()) << shallow.error().to_string();
    EXPECT_EQ(shallow.value(), 3);

    auto max_score = scorer.value()->get_max_score(4);
    ASSERT_TRUE(max_score.has_value());
    EXPECT_FLOAT_EQ(max_score.value(), 2 * kFreqWeight + 5.0F);

    // The scorer is not the top-level clause, so the threshold is ignored.
    EXPECT_TRUE(scorer.value()->set_min_competitive_score(1000.0F).has_value());
    EXPECT_EQ(drain(*scorer.value()), (std::vector<int32_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
}

TEST(ImpactsTest, TermScorerTopLevelClausePrunesWithTheCompetitiveScore) {
    auto scorer = TermScorer::from_impacts(std::make_unique<BlockImpactsEnum>(),
                                          std::make_shared<LinearSimScorer>(), nullptr, true);
    ASSERT_TRUE(scorer.has_value()) << scorer.error().to_string();

    EXPECT_TRUE(scorer.value()->set_min_competitive_score(20.0F).has_value());
    EXPECT_EQ(drain(*scorer.value()), (std::vector<int32_t>{4, 5, 6, 7, 8, 9}));
}

TEST(ImpactsTest, TermScorerWithoutImpactsCannotPrune) {
    // A plain postings enum keeps the "no impacts" behaviour: +infinity as the
    // maximum score and no shallow advances.
    class PlainPostings final : public PostingsEnum {
    public:
        [[nodiscard]] int32_t doc_id() const override { return doc_; }
        [[nodiscard]] Result<int32_t> next_doc() override { return advance(doc_ + 1); }
        [[nodiscard]] Result<int32_t> advance(int32_t target) override {
            doc_ = target < 3 ? target : NO_MORE_DOCS;
            return doc_;
        }
        [[nodiscard]] Result<int64_t> cost() const override { return 3; }
        [[nodiscard]] Result<int32_t> freq() override { return 1; }
        [[nodiscard]] Result<int32_t> next_position() override {
            return err(LuceneError::unsupported_operation("no positions"));
        }
        [[nodiscard]] Result<int32_t> start_offset() const override {
            return err(LuceneError::unsupported_operation("no offsets"));
        }
        [[nodiscard]] Result<int32_t> end_offset() const override {
            return err(LuceneError::unsupported_operation("no offsets"));
        }
        [[nodiscard]] Result<std::optional<std::span<const uint8_t>>> get_payload() const override {
            return std::nullopt;
        }

    private:
        int32_t doc_{-1};
    };

    TermScorer scorer(std::make_unique<PlainPostings>(), std::make_shared<LinearSimScorer>(), nullptr);
    EXPECT_FALSE(scorer.has_impacts());
    EXPECT_EQ(scorer.get_max_score(0).value(), std::numeric_limits<float>::infinity());
    EXPECT_EQ(scorer.advance_shallow(0).value(), NO_MORE_DOCS);
    EXPECT_TRUE(scorer.set_min_competitive_score(1000.0F).has_value());
    EXPECT_EQ(drain(scorer), (std::vector<int32_t>{0, 1, 2}));
}

TEST(ImpactsTest, SearchOverImpactsReturnsTheTopScores) {
    IndexSearcher searcher(std::make_shared<ImpactsLeafReader>(), std::make_shared<LinearSimilarity>());
    const TermQuery query(Term::from_text("body", "foo"));

    // Block `b` scores `(b + 1) * 10 + 1`, so documents 8 and 9 win and the
    // best of the second block is document 4.
    auto top = searcher.search(query, 3);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    ASSERT_EQ(top.value().score_docs.size(), 3U);
    EXPECT_EQ(top.value().score_docs[0].doc, 8);
    EXPECT_EQ(top.value().score_docs[1].doc, 9);
    EXPECT_EQ(top.value().score_docs[2].doc, 4);
    EXPECT_FLOAT_EQ(top.value().score_docs[0].score, 31.0F);
    EXPECT_FLOAT_EQ(top.value().score_docs[2].score, 21.0F);
    EXPECT_EQ(top.value().total_hits.value, static_cast<size_t>(kMaxDoc));
}

namespace {

/// A scorer that only serves the block-boundary propagator: fixed maximum score,
/// fixed shallow-advance answer and a fixed document.
class PropagatorScorer final : public Scorer {
public:
    PropagatorScorer(float max_score, int64_t cost, int32_t doc, int32_t shallow_upto)
            : max_score_(max_score), cost_(cost), doc_(doc), shallow_upto_(shallow_upto) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override { return doc_; }
    [[nodiscard]] Result<int32_t> advance(int32_t /*target*/) override { return doc_; }
    [[nodiscard]] Result<int64_t> cost() const override { return cost_; }
    [[nodiscard]] Result<int32_t> scoring_doc_id() override { return doc_; }
    [[nodiscard]] Result<float> get_max_score(int32_t /*upto*/) override { return max_score_; }
    [[nodiscard]] TwoPhaseState has_two_phase_iterator() const override { return TwoPhaseState::No; }
    [[nodiscard]] Result<float> score() override { return 0.0F; }

    [[nodiscard]] Result<int32_t> advance_shallow(int32_t target) override {
        last_shallow_target_ = target;
        return shallow_upto_;
    }

    [[nodiscard]] int32_t last_shallow_target() const noexcept { return last_shallow_target_; }

private:
    float max_score_;
    int64_t cost_;
    int32_t doc_;
    int32_t shallow_upto_;
    int32_t last_shallow_target_{-1};
};

} // namespace

TEST(ImpactsTest, BlockBoundaryPropagatorOrdersClausesByScoreThenCost) {
    PropagatorScorer low(10.0F, 100, 0, 7);
    PropagatorScorer middle(20.0F, 50, 0, 9);
    PropagatorScorer high(30.0F, 10, 12, 15);
    std::vector<Scorer *> scorers{&high, &low, &middle};

    auto propagator = DisjunctionScoreBlockBoundaryPropagator::create(scorers);
    ASSERT_TRUE(propagator.has_value()) << propagator.error().to_string();
    const auto &costs = propagator.value().costs();
    ASSERT_EQ(costs.size(), 3U);
    EXPECT_FLOAT_EQ(costs[0].max_score, 10.0F);
    EXPECT_EQ(costs[0].idx, 1U);
    EXPECT_FLOAT_EQ(costs[1].max_score, 20.0F);
    EXPECT_EQ(costs[1].idx, 2U);
    EXPECT_FLOAT_EQ(costs[2].max_score, 30.0F);
    EXPECT_EQ(costs[2].idx, 0U);
    EXPECT_EQ(propagator.value().lead_index(), 0U);
}

TEST(ImpactsTest, BlockBoundaryPropagatorTakesTheMinimumOfCompetitiveClauses) {
    PropagatorScorer low(10.0F, 100, 0, 7);
    PropagatorScorer middle(20.0F, 50, 0, 9);
    PropagatorScorer high(30.0F, 10, 12, 15);
    std::vector<Scorer *> scorers{&low, &middle, &high};

    auto propagator = DisjunctionScoreBlockBoundaryPropagator::create(scorers);
    ASSERT_TRUE(propagator.has_value());

    auto upto = propagator.value().advance_shallow(5, scorers);
    ASSERT_TRUE(upto.has_value()) << upto.error().to_string();
    EXPECT_EQ(upto.value(), 7);
    EXPECT_EQ(low.last_shallow_target(), 5);
    EXPECT_EQ(middle.last_shallow_target(), 5);
    // The maximum-scoring clause is beyond the target, so it is only shallow
    // advanced by `create` (to 0); it bounds the block by its own document.
    EXPECT_EQ(high.last_shallow_target(), 0);
}

TEST(ImpactsTest, BlockBoundaryPropagatorDropsClausesBelowTheCompetitiveScore) {
    PropagatorScorer low(10.0F, 100, 0, 7);
    PropagatorScorer middle(20.0F, 50, 0, 9);
    PropagatorScorer high(30.0F, 10, 12, 15);
    std::vector<Scorer *> scorers{&low, &middle, &high};

    auto propagator = DisjunctionScoreBlockBoundaryPropagator::create(scorers);
    ASSERT_TRUE(propagator.has_value());

    // The low-scoring clause can no longer compete, so it only gets propagated
    // and the block boundary comes from the middle clause.
    propagator.value().set_min_competitive_score(15.0F);
    EXPECT_EQ(propagator.value().lead_index(), 1U);

    auto upto = propagator.value().advance_shallow(5, scorers);
    ASSERT_TRUE(upto.has_value()) << upto.error().to_string();
    EXPECT_EQ(upto.value(), 9);
    EXPECT_EQ(low.last_shallow_target(), 5);

    // A threshold above every clause leaves only the maximum-scoring one.
    propagator.value().set_min_competitive_score(1'000.0F);
    EXPECT_EQ(propagator.value().lead_index(), 2U);
}

TEST(ImpactsTest, BooleanQueryOverImpactsUsesBlockMaxConjunction) {
    IndexSearcher searcher(std::make_shared<ImpactsLeafReader>(), std::make_shared<LinearSimilarity>());

    BooleanQuery::Builder builder;
    ASSERT_TRUE(builder.add(std::make_shared<TermQuery>(Term::from_text("body", "foo")), Occur::Must).has_value());
    ASSERT_TRUE(builder.add(std::make_shared<TermQuery>(Term::from_text("body", "foo")), Occur::Must).has_value());
    auto query = builder.build();
    ASSERT_TRUE(query.has_value()) << query.error().to_string();

    // Both clauses match every document, so the scores are doubled and no
    // document is pruned away.
    auto top = searcher.search(query.value(), 10);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    EXPECT_EQ(top.value().total_hits.value, static_cast<size_t>(kMaxDoc));
    ASSERT_EQ(top.value().score_docs.size(), static_cast<size_t>(kMaxDoc));
    EXPECT_EQ(top.value().score_docs[0].doc, 8);
    EXPECT_FLOAT_EQ(top.value().score_docs[0].score, 62.0F);
    EXPECT_FLOAT_EQ(top.value().score_docs[5].score, 42.0F);
}

TEST(ImpactsTest, BooleanQueryWithShouldClausesUsesMaxScore) {
    IndexSearcher searcher(std::make_shared<ImpactsLeafReader>(), std::make_shared<LinearSimilarity>());

    // Two SHOULD clauses: `ScoreMode::TopScores` lets the search use MAXSCORE,
    // so the two clauses are partitioned into essential and non-essential ones.
    BooleanQuery::Builder builder;
    ASSERT_TRUE(builder.add(std::make_shared<TermQuery>(Term::from_text("body", "foo")), Occur::Should).has_value());
    ASSERT_TRUE(builder.add(std::make_shared<TermQuery>(Term::from_text("body", "foo")), Occur::Should).has_value());
    auto query = builder.build();
    ASSERT_TRUE(query.has_value()) << query.error().to_string();

    auto top = searcher.search(query.value(), 10);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    EXPECT_EQ(top.value().total_hits.value, static_cast<size_t>(kMaxDoc));
    ASSERT_EQ(top.value().score_docs.size(), static_cast<size_t>(kMaxDoc));
    EXPECT_EQ(top.value().score_docs[0].doc, 8);
    EXPECT_FLOAT_EQ(top.value().score_docs[0].score, 62.0F);
}
