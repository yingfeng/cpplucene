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

#include <cmath>
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

module lucene_core:ut.term_query;

import lucene_core;

using namespace lucene;

namespace {

/// One posting: a document and its term frequency.
struct Posting {
    int32_t doc;
    int32_t freq;
};

/// [`PostingsEnum`] over a fixed list of postings.
class VectorPostingsEnum final : public PostingsEnum {
public:
    explicit VectorPostingsEnum(std::vector<Posting> postings) : postings_(std::move(postings)) {}

    [[nodiscard]] int32_t doc_id() const override {
        if (index_ < 0) {
            return -1;
        }
        return index_ < size() ? postings_[static_cast<size_t>(index_)].doc : NO_MORE_DOCS;
    }

    [[nodiscard]] Result<int32_t> next_doc() override {
        ++index_;
        return doc_id();
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        if (index_ >= 0 && doc_id() >= target) {
            return doc_id();
        }
        do {
            ++index_;
        } while (doc_id() < target && doc_id() != NO_MORE_DOCS);
        return doc_id();
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(size()); }

    [[nodiscard]] Result<int32_t> freq() override {
        if (index_ < 0 || index_ >= size()) {
            return err(LuceneError::illegal_state("postings are not positioned"));
        }
        return postings_[static_cast<size_t>(index_)].freq;
    }

    [[nodiscard]] Result<int32_t> next_position() override {
        return err(LuceneError::unsupported_operation("positions are not indexed"));
    }
    [[nodiscard]] Result<int32_t> start_offset() const override {
        return err(LuceneError::unsupported_operation("offsets are not indexed"));
    }
    [[nodiscard]] Result<int32_t> end_offset() const override {
        return err(LuceneError::unsupported_operation("offsets are not indexed"));
    }
    [[nodiscard]] Result<std::optional<std::span<const uint8_t>>> get_payload() const override { return std::nullopt; }

private:
    [[nodiscard]] size_t size() const noexcept { return postings_.size(); }

    std::vector<Posting> postings_;
    int32_t index_{-1};
};

/// [`NumericDocValues`] over a fixed per-document array.
class ArrayNumericDocValues final : public NumericDocValues {
public:
    explicit ArrayNumericDocValues(std::vector<int64_t> values) : values_(std::move(values)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override { return advance(doc_ + 1); }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        int32_t doc = target;
        while (doc < static_cast<int32_t>(values_.size()) && values_[static_cast<size_t>(doc)] == kMissing) {
            ++doc;
        }
        doc_ = doc < static_cast<int32_t>(values_.size()) ? doc : NO_MORE_DOCS;
        return doc_;
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(values_.size()); }

    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        auto advanced = advance(target);
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        return advanced.value() == target;
    }

    [[nodiscard]] Result<int64_t> long_value() override {
        if (doc_ < 0 || doc_ >= static_cast<int32_t>(values_.size())) {
            return err(LuceneError::illegal_state("doc values are not positioned"));
        }
        return values_[static_cast<size_t>(doc_)];
    }

    /// Marks a document as having no value.
    static constexpr int64_t kMissing = std::numeric_limits<int64_t>::min();

private:
    std::vector<int64_t> values_;
    int32_t doc_{-1};
};

/// Per-term postings of the fake field.
struct TermPostings {
    std::vector<Posting> postings;
    int64_t total_term_freq{0};
};

/// [`TermsEnum`] over an ordered map of term text to postings.
class MapTermsEnum final : public TermsEnum {
public:
    explicit MapTermsEnum(std::map<std::string, TermPostings> terms) : terms_(std::move(terms)) {}

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

    [[nodiscard]] Result<AttributeSource *> attributes() override {
        return err(LuceneError::unsupported_operation("MapTermsEnum has no attributes"));
    }
    [[nodiscard]] Result<AttributeSource *> attributes_mut() override {
        return err(LuceneError::unsupported_operation("MapTermsEnum has no attributes"));
    }

    [[nodiscard]] Result<SeekStatus> seek_ceil(std::span<const uint8_t> term) override {
        const std::string text(reinterpret_cast<const char *>(term.data()), term.size());
        for (size_t i = 0; i < terms_.size(); ++i) {
            const auto &[candidate, postings] = *std::next(terms_.begin(), static_cast<std::ptrdiff_t>(i));
            if (candidate < text) {
                continue;
            }
            pos_ = i;
            return candidate == text ? SeekStatus::Found : SeekStatus::NotFound;
        }
        pos_.reset();
        return SeekStatus::End;
    }

    [[nodiscard]] Result<bool> seek_exact(std::span<const uint8_t> term) override {
        auto status = seek_ceil(term);
        if (!status.has_value()) {
            return std::unexpected(std::move(status.error()));
        }
        return status.value() == SeekStatus::Found;
    }

    [[nodiscard]] Result<bool> prepare_seek_exact(std::span<const uint8_t> term) override {
        prepared_ = std::string(reinterpret_cast<const char *>(term.data()), term.size());
        return true;
    }

    [[nodiscard]] Result<bool> get_prepare_seek_exact_status(std::span<const uint8_t> term) override {
        if (!prepared_.has_value()) {
            return err(LuceneError::illegal_state("prepare_seek_exact was not called"));
        }
        return seek_exact(term);
    }

    [[nodiscard]] VoidResult seek_exact_with_ord(int64_t ord) override {
        if (ord < 0 || static_cast<size_t>(ord) >= terms_.size()) {
            return err(LuceneError::illegal_argument("ord out of range"));
        }
        pos_ = static_cast<size_t>(ord);
        return {};
    }

    /// The fake term state is the term's ordinal, which is what the index of a
    /// sorted map gives.
    [[nodiscard]] VoidResult seek_exact_with_state(std::span<const uint8_t> term,
                                                   const TermStateEnum &state) override {
        auto ord = state.ord();
        if (!ord.has_value()) {
            return std::unexpected(std::move(ord.error()));
        }
        auto positioned = seek_exact_with_ord(ord.value());
        if (!positioned.has_value()) {
            return positioned;
        }
        auto current = this->term();
        if (!current.has_value()) {
            return std::unexpected(std::move(current.error()));
        }
        if (std::ranges::equal(current.value()->as_slice(), term)) {
            return {};
        }
        return err(LuceneError::illegal_state("term state points at another term"));
    }

    [[nodiscard]] Result<const BytesRef *> term() const override {
        if (!pos_.has_value()) {
            return err(LuceneError::illegal_state("unpositioned"));
        }
        return &term_at(pos_.value());
    }

    [[nodiscard]] Result<int64_t> ord() const override { return static_cast<int64_t>(pos_.value_or(0)); }

    [[nodiscard]] Result<int32_t> doc_freq() override {
        if (!pos_.has_value()) {
            return err(LuceneError::illegal_state("unpositioned"));
        }
        return static_cast<int32_t>(postings_at(pos_.value()).postings.size());
    }

    [[nodiscard]] Result<int64_t> total_term_freq() override {
        if (!pos_.has_value()) {
            return err(LuceneError::illegal_state("unpositioned"));
        }
        return postings_at(pos_.value()).total_term_freq;
    }

    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>> postings_with_flags(std::unique_ptr<PostingsEnum> /*reuse*/,
                                                                           int32_t /*flags*/) override {
        if (!pos_.has_value()) {
            return err(LuceneError::illegal_state("unpositioned"));
        }
        return std::unique_ptr<PostingsEnum>(std::make_unique<VectorPostingsEnum>(postings_at(pos_.value()).postings));
    }

    [[nodiscard]] Result<std::unique_ptr<ImpactsEnum>> impacts(int32_t /*flags*/) override {
        return err(LuceneError::unsupported_operation("MapTermsEnum has no impacts"));
    }

    [[nodiscard]] Result<TermStateEnum> term_state() override {
        if (!pos_.has_value()) {
            return err(LuceneError::illegal_state("unpositioned"));
        }
        return TermStateEnum(OrdTermState(static_cast<int64_t>(pos_.value())));
    }

private:
    [[nodiscard]] const BytesRef &term_at(size_t index) const {
        auto it = terms_.begin();
        std::advance(it, static_cast<std::ptrdiff_t>(index));
        cache_ = BytesRef::from_string(it->first);
        return cache_;
    }

    [[nodiscard]] const TermPostings &postings_at(size_t index) const {
        auto it = terms_.begin();
        std::advance(it, static_cast<std::ptrdiff_t>(index));
        return it->second;
    }

    std::map<std::string, TermPostings> terms_;
    std::optional<size_t> pos_{};
    std::optional<std::string> prepared_{};
    mutable BytesRef cache_{};
};

/// [`Terms`] over the fake field.
class MapTerms final : public Terms {
public:
    explicit MapTerms(std::map<std::string, TermPostings> terms) : terms_(std::move(terms)) {}

    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> iterator() const override {
        return std::unique_ptr<TermsEnum>(std::make_unique<MapTermsEnum>(terms_));
    }

    [[nodiscard]] Result<int64_t> size() const override { return static_cast<int64_t>(terms_.size()); }

    [[nodiscard]] Result<int64_t> sum_total_term_freq() const override {
        int64_t sum = 0;
        for (const auto &[text, postings] : terms_) {
            sum += postings.total_term_freq;
        }
        return sum;
    }

    [[nodiscard]] Result<int64_t> sum_doc_freq() const override {
        int64_t sum = 0;
        for (const auto &[text, postings] : terms_) {
            sum += static_cast<int64_t>(postings.postings.size());
        }
        return sum;
    }

    [[nodiscard]] Result<int32_t> doc_count() const override {
        int32_t count = 0;
        for (const auto &[text, postings] : terms_) {
            count += static_cast<int32_t>(postings.postings.size());
        }
        return count;
    }

    [[nodiscard]] bool has_freqs() const override { return true; }
    [[nodiscard]] bool has_offsets() const override { return false; }
    [[nodiscard]] bool has_positions() const override { return false; }
    [[nodiscard]] bool has_payloads() const override { return false; }

private:
    std::map<std::string, TermPostings> terms_;
};

/// A [`LeafReader`] with one terms field and optional norms.
class PostingsLeafReader final : public LeafReader {
public:
    PostingsLeafReader(std::string field, std::map<std::string, TermPostings> terms, std::vector<int64_t> norms,
                       int32_t max_doc, int32_t num_docs)
        : field_(std::move(field)),
          terms_(std::move(terms)),
          norms_(std::move(norms)),
          max_doc_(max_doc),
          num_docs_(num_docs) {}

    [[nodiscard]] Result<int32_t> max_doc() const override { return max_doc_; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return num_docs_; }

    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override { return nullptr; }
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override { return nullptr; }

    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view field) override {
        if (field != field_) {
            return std::unique_ptr<Terms>{};
        }
        return std::unique_ptr<Terms>(std::make_unique<MapTerms>(terms_));
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

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norm_values(std::string_view field) override {
        if (field != field_ || norms_.empty()) {
            return std::unique_ptr<NumericDocValues>{};
        }
        return std::unique_ptr<NumericDocValues>(std::make_unique<ArrayNumericDocValues>(norms_));
    }

    [[nodiscard]] Result<std::unique_ptr<DocValuesSkipper>> get_doc_values_skipper(std::string_view /*field*/) override {
        return nullptr;
    }

    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return &field_infos_; }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return static_cast<const Bits *>(nullptr); }

private:
    std::string field_;
    std::map<std::string, TermPostings> terms_;
    std::vector<int64_t> norms_;
    int32_t max_doc_;
    int32_t num_docs_;
    FieldInfos field_infos_{FieldInfos::none()};
};

/// A similarity whose scorer is `freq * 10 + norm`, so tests can assert exact
/// values without reimplementing BM25.
class FixedSimScorer final : public SimScorer {
public:
    [[nodiscard]] float score(float freq, int64_t norm) const override {
        return freq * 10.0F + static_cast<float>(norm);
    }
};

class FixedSimilarity final : public Similarity {
public:
    [[nodiscard]] Result<std::unique_ptr<SimScorer>> scorer(
            float /*boost*/, const CollectionStatistics & /*collection_stats*/,
            std::span<const TermStatistics> /*term_stats*/) const override {
        return std::unique_ptr<SimScorer>(std::make_unique<FixedSimScorer>());
    }

    [[nodiscard]] std::string name() const override { return "FixedSimilarity"; }
};

/// Two terms: "foo" in docs 1 and 3, "bar" in doc 2.  Norms encode `doc + 1`.
std::shared_ptr<IndexReader> make_reader(int32_t max_doc = 4, int32_t num_docs = 4) {
    std::map<std::string, TermPostings> terms;
    terms["bar"] = TermPostings{{Posting{2, 5}}, 5};
    terms["foo"] = TermPostings{{Posting{1, 2}, Posting{3, 4}}, 6};
    return std::make_shared<PostingsLeafReader>("body", std::move(terms),
                                                std::vector<int64_t>{ArrayNumericDocValues::kMissing, 7, 8, 9},
                                                max_doc, num_docs);
}

IndexSearcher make_searcher(std::shared_ptr<IndexReader> reader) {
    return IndexSearcher(std::move(reader), std::make_shared<FixedSimilarity>());
}

/// Drains a scorer into the list of documents it matches.
std::vector<int32_t> drain(Scorer &scorer) {
    std::vector<int32_t> docs;
    while (true) {
        auto doc = scorer.next_doc();
        EXPECT_TRUE(doc.has_value());
        if (doc.value() == NO_MORE_DOCS) {
            break;
        }
        docs.push_back(doc.value());
    }
    return docs;
}

/// Collects the terms a query hands to a [`QueryVisitor`].
class CollectingVisitor final : public QueryVisitor {
public:
    std::vector<std::string> terms;
    int32_t leaves{0};
    std::string accepted_field = "body";

    [[nodiscard]] bool accept_field(std::string_view field) const override {
        return field == accepted_field;
    }

    [[nodiscard]] VoidResult consume_terms(const Query &query, std::span<const Term> terms_in) override {
        (void)query;
        for (const auto &term : terms_in) {
            terms.push_back(term.text());
        }
        return {};
    }

    [[nodiscard]] VoidResult visit_leaf(const Query &query) override {
        (void)query;
        ++leaves;
        return {};
    }
};

} // namespace

TEST(TermQueryTest, IteratesMatchingDocuments) {
    auto searcher = make_searcher(make_reader());
    const TermQuery query(Term::from_text("body", "foo"));

    auto weight = query.create_weight(searcher, ScoreMode::CompleteNoScores, 1.0F);
    ASSERT_TRUE(weight.has_value());
    EXPECT_EQ(&(*weight)->get_query(), &query);

    auto leaves = searcher.leaves().value();
    auto scorer = (*weight)->scorer(leaves[0], searcher);
    ASSERT_TRUE(scorer.has_value());
    ASSERT_TRUE(scorer.value() != nullptr);
    EXPECT_EQ(drain(*scorer.value()), (std::vector<int32_t>{1, 3}));
}

TEST(TermQueryTest, ScoresWithTheSearcherSimilarity) {
    auto searcher = make_searcher(make_reader());
    const TermQuery query(Term::from_text("body", "foo"));

    auto weight = query.create_weight(searcher, ScoreMode::Complete, 1.0F);
    ASSERT_TRUE(weight.has_value());

    auto leaves = searcher.leaves().value();
    auto scorer = (*weight)->scorer(leaves[0], searcher);
    ASSERT_TRUE(scorer.has_value());
    ASSERT_TRUE(scorer.value() != nullptr);

    // Doc 1: freq 2, norm 7 → 2 * 10 + 7.
    auto doc = scorer.value()->next_doc();
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc.value(), 1);
    auto score = scorer.value()->score();
    ASSERT_TRUE(score.has_value());
    EXPECT_FLOAT_EQ(score.value(), 27.0F);

    // Doc 3: freq 4, norm 9 → 4 * 10 + 9.
    doc = scorer.value()->next_doc();
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc.value(), 3);
    score = scorer.value()->score();
    ASSERT_TRUE(score.has_value());
    EXPECT_FLOAT_EQ(score.value(), 49.0F);
}

TEST(TermQueryTest, ScoresAreZeroWhenScoresAreNotNeeded) {
    auto searcher = make_searcher(make_reader());
    const TermQuery query(Term::from_text("body", "foo"));

    auto weight = query.create_weight(searcher, ScoreMode::CompleteNoScores, 1.0F);
    ASSERT_TRUE(weight.has_value());
    auto leaves = searcher.leaves().value();
    auto scorer = (*weight)->scorer(leaves[0], searcher);
    ASSERT_TRUE(scorer.has_value());
    ASSERT_TRUE(scorer.value()->next_doc().has_value());
    auto score = scorer.value()->score();
    ASSERT_TRUE(score.has_value());
    EXPECT_FLOAT_EQ(score.value(), 0.0F);
}

TEST(TermQueryTest, SupplierCostIsTheDocumentFrequency) {
    auto searcher = make_searcher(make_reader());
    const TermQuery query(Term::from_text("body", "foo"));

    auto weight = query.create_weight(searcher, ScoreMode::Complete, 1.0F);
    ASSERT_TRUE(weight.has_value());
    auto leaves = searcher.leaves().value();
    auto supplier = (*weight)->scorer_supplier(leaves[0], searcher);
    ASSERT_TRUE(supplier.has_value());
    ASSERT_TRUE(supplier.value() != nullptr);
    EXPECT_EQ(supplier.value()->cost(leaves[0], searcher).value(), 2);
}

TEST(TermQueryTest, CountUsesDocumentFrequencyWhenThereAreNoDeletions) {
    auto searcher = make_searcher(make_reader());
    const TermQuery query(Term::from_text("body", "foo"));

    auto weight = query.create_weight(searcher, ScoreMode::Complete, 1.0F);
    ASSERT_TRUE(weight.has_value());
    auto leaves = searcher.leaves().value();
    EXPECT_EQ((*weight)->count(leaves[0], searcher).value(), 2);
}

TEST(TermQueryTest, CountFallsBackWhenThereAreDeletions) {
    // 4 documents, only 3 live: the sub-linear count is not valid any more.
    auto searcher = make_searcher(make_reader(/*max_doc=*/4, /*num_docs=*/3));
    const TermQuery query(Term::from_text("body", "foo"));

    auto weight = query.create_weight(searcher, ScoreMode::Complete, 1.0F);
    ASSERT_TRUE(weight.has_value());
    auto leaves = searcher.leaves().value();
    EXPECT_EQ((*weight)->count(leaves[0], searcher).value(), -1);
}

TEST(TermQueryTest, AbsentTermHasNoSupplier) {
    auto searcher = make_searcher(make_reader());
    const TermQuery query(Term::from_text("body", "zoo"));

    auto weight = query.create_weight(searcher, ScoreMode::Complete, 1.0F);
    ASSERT_TRUE(weight.has_value());
    auto leaves = searcher.leaves().value();

    auto supplier = (*weight)->scorer_supplier(leaves[0], searcher);
    ASSERT_TRUE(supplier.has_value());
    EXPECT_TRUE(supplier.value() == nullptr);

    auto scorer = (*weight)->scorer(leaves[0], searcher);
    ASSERT_TRUE(scorer.has_value());
    EXPECT_TRUE(scorer.value() == nullptr);

    EXPECT_EQ((*weight)->count(leaves[0], searcher).value(), 0);
}

TEST(TermQueryTest, ExplainReportsTheScore) {
    auto searcher = make_searcher(make_reader());
    const TermQuery query(Term::from_text("body", "foo"));
    auto weight = query.create_weight(searcher, ScoreMode::Complete, 1.0F);
    ASSERT_TRUE(weight.has_value());
    auto leaves = searcher.leaves().value();

    auto explanation = (*weight)->explain(leaves[0], 1, searcher);
    ASSERT_TRUE(explanation.has_value());
    EXPECT_TRUE(explanation.value().is_match());
    EXPECT_FLOAT_EQ(std::get<float>(explanation.value().value()), 27.0F);
    EXPECT_NE(explanation.value().description().find("weight(body:foo in 1) [FixedSimilarity]"), std::string::npos);
    ASSERT_EQ(explanation.value().details().size(), 1U);

    // A document without the term does not match.
    auto absent = (*weight)->explain(leaves[0], 0, searcher);
    ASSERT_TRUE(absent.has_value());
    EXPECT_FALSE(absent.value().is_match());
    EXPECT_EQ(absent.value().description(), "no matching term");
}

TEST(TermQueryTest, BulkScorerIteratesThroughTheDefaultWrapper) {
    auto searcher = make_searcher(make_reader());
    const TermQuery query(Term::from_text("body", "foo"));
    auto weight = query.create_weight(searcher, ScoreMode::Complete, 1.0F);
    ASSERT_TRUE(weight.has_value());
    auto leaves = searcher.leaves().value();

    auto bulk = (*weight)->bulk_scorer(leaves[0], searcher);
    ASSERT_TRUE(bulk.has_value());
    ASSERT_TRUE(bulk.value() != nullptr);
    EXPECT_EQ(bulk.value()->cost().value(), 2);
}

TEST(TermQueryTest, ToStringEqualsAndHash) {
    const TermQuery first(Term::from_text("body", "foo"));
    const TermQuery same(Term::from_text("body", "foo"));
    const TermQuery other_field(Term::from_text("title", "foo"));
    const TermQuery other_term(Term::from_text("body", "bar"));

    EXPECT_EQ(first.to_string("body").value(), "foo");
    EXPECT_EQ(first.to_string("title").value(), "body:foo");
    EXPECT_EQ(first.to_string("").value(), "body:foo");

    EXPECT_TRUE(first.equals(same));
    EXPECT_FALSE(first.equals(other_field));
    EXPECT_FALSE(first.equals(other_term));
    EXPECT_EQ(first.hash(), same.hash());

    // Different instances, different identities.
    EXPECT_FALSE(first.identity() == same.identity());
}

TEST(TermQueryTest, VisitHandsTheTermToInterestedVisitors) {
    const TermQuery query(Term::from_text("body", "foo"));

    CollectingVisitor visitor;
    ASSERT_TRUE(query.visit(visitor).has_value());
    EXPECT_EQ(visitor.terms, (std::vector<std::string>{"foo"}));
    EXPECT_EQ(visitor.leaves, 0);

    // A visitor that filters the field out gets nothing.
    CollectingVisitor uninterested;
    uninterested.accepted_field = "title";
    ASSERT_TRUE(query.visit(uninterested).has_value());
    EXPECT_TRUE(uninterested.terms.empty());
}

TEST(TermQueryTest, QueryReusesTermStatesBuiltForTheSameReader) {
    auto reader = make_reader();
    auto searcher = IndexSearcher(reader, std::make_shared<FixedSimilarity>());

    auto states = build_term_states(searcher, Term::from_text("body", "foo"), /*needs_stats=*/true);
    ASSERT_TRUE(states.has_value());
    auto shared_states = std::make_shared<TermStates>(std::move(states.value()));
    const TermQuery query(Term::from_text("body", "foo"), shared_states);

    ASSERT_EQ(query.term_states(), shared_states);
    auto weight = query.create_weight(searcher, ScoreMode::Complete, 1.0F);
    ASSERT_TRUE(weight.has_value());
    auto *term_weight = dynamic_cast<TermWeight *>(weight->get());
    ASSERT_TRUE(term_weight != nullptr);
    // The weight adopted the caller's states instead of rebuilding them.
    EXPECT_EQ(term_weight->term_states(), shared_states);
}
