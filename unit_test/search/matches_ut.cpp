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

module lucene_core:ut.matches;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kField = "body";

/// One match of a fake [`MatchesIterator`].
struct MatchSpan {
    int32_t start_position;
    int32_t end_position;
    int32_t start_offset;
    int32_t end_offset;
};

/// [`MatchesIterator`] over a fixed list of matches.
class FakeMatchesIterator final : public MatchesIterator {
public:
    explicit FakeMatchesIterator(std::vector<MatchSpan> spans) : spans_(std::move(spans)) {}

    [[nodiscard]] Result<bool> next() override {
        ++index_;
        return index_ < static_cast<int32_t>(spans_.size());
    }

    [[nodiscard]] Result<int32_t> start_position() const override { return current().start_position; }
    [[nodiscard]] Result<int32_t> end_position() const override { return current().end_position; }
    [[nodiscard]] Result<int32_t> start_offset() const override { return current().start_offset; }
    [[nodiscard]] Result<int32_t> end_offset() const override { return current().end_offset; }

    [[nodiscard]] Result<std::unique_ptr<MatchesIterator>> get_sub_matches() override {
        return std::unique_ptr<MatchesIterator>{};
    }

    [[nodiscard]] Result<std::shared_ptr<Query>> get_query() const override { return std::shared_ptr<Query>{}; }

private:
    [[nodiscard]] const MatchSpan &current() const {
        static const MatchSpan kNone{-1, -1, -1, -1};
        if (index_ < 0 || index_ >= static_cast<int32_t>(spans_.size())) {
            return kNone;
        }
        return spans_[static_cast<size_t>(index_)];
    }

    std::vector<MatchSpan> spans_;
    int32_t index_{-1};
};

/// [`Matches`] that reports the same matches for one field.
class FakeMatches final : public Matches {
public:
    FakeMatches(std::string field, std::vector<MatchSpan> spans)
            : field_(std::move(field)), spans_(std::move(spans)) {}

    [[nodiscard]] Result<std::unique_ptr<MatchesIterator>> get_matches(std::string_view field) const override {
        if (field != field_) {
            return std::unique_ptr<MatchesIterator>{};
        }
        return std::unique_ptr<MatchesIterator>(std::make_unique<FakeMatchesIterator>(spans_));
    }

    [[nodiscard]] std::vector<std::string> fields() const override { return {field_}; }

private:
    std::string field_;
    std::vector<MatchSpan> spans_;
};

/// Drains an iterator into the list of start positions it reports.
std::vector<int32_t> drain_positions(MatchesIterator &iterator) {
    std::vector<int32_t> positions;
    while (true) {
        auto next = iterator.next();
        EXPECT_TRUE(next.has_value());
        if (!next.value()) {
            break;
        }
        auto position = iterator.start_position();
        EXPECT_TRUE(position.has_value());
        positions.push_back(position.value());
    }
    return positions;
}

// ------------------------------------------------- Weight::matches defaults

/// A document with its score, the only thing the fake scorers need.
struct Hit {
    int32_t doc;
    float score;
};

/// A scorer over a fixed list of documents; with `confirmed` set it only really
/// matches the listed documents and reports itself as two-phase.
class FixedScorer final : public Scorer {
public:
    FixedScorer(std::vector<Hit> hits, std::vector<int32_t> confirmed)
            : hits_(std::move(hits)), confirmed_(std::move(confirmed)) {}

    [[nodiscard]] int32_t doc_id() const override {
        return index_ < hits_.size() ? hits_[index_].doc : NO_MORE_DOCS;
    }

    [[nodiscard]] Result<int32_t> next_doc() override {
        ++index_;
        return doc_id();
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        while (doc_id() < target && doc_id() != NO_MORE_DOCS) {
            ++index_;
        }
        return doc_id();
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(hits_.size()); }

    [[nodiscard]] Result<int32_t> scoring_doc_id() override { return doc_id(); }

    [[nodiscard]] Result<float> get_max_score(int32_t /*upto*/) override {
        return std::numeric_limits<float>::infinity();
    }

    [[nodiscard]] TwoPhaseState has_two_phase_iterator() const override {
        return confirmed_.empty() ? TwoPhaseState::No : TwoPhaseState::Yes;
    }

    [[nodiscard]] Result<bool> matches() override {
        return std::ranges::find(confirmed_, doc_id()) != confirmed_.end();
    }

    [[nodiscard]] Result<float> score() override { return 1.0F; }

private:
    std::vector<Hit> hits_;
    std::vector<int32_t> confirmed_;
    size_t index_{0};
};

class FixedScorerSupplier final : public ScorerSupplier {
public:
    FixedScorerSupplier(std::vector<Hit> hits, std::vector<int32_t> confirmed)
            : hits_(std::move(hits)), confirmed_(std::move(confirmed)) {}

    [[nodiscard]] Result<std::unique_ptr<Scorer>> get(int64_t /*lead_cost*/, const LeafReaderContext & /*context*/,
                                                      const IndexSearcher & /*searcher*/) override {
        return std::unique_ptr<Scorer>(new FixedScorer(hits_, confirmed_));
    }

    [[nodiscard]] Result<int64_t> cost(const LeafReaderContext & /*context*/,
                                       const IndexSearcher & /*searcher*/) override {
        return static_cast<int64_t>(hits_.size());
    }

private:
    std::vector<Hit> hits_;
    std::vector<int32_t> confirmed_;
};

class FixedQuery;

/// A weight whose scorer comes from a fixed list of hits; `matches` is the
/// default [`Weight::matches`].
class FixedWeight final : public Weight {
public:
    FixedWeight(const FixedQuery &query, std::vector<Hit> hits, std::vector<int32_t> confirmed)
            : query_(query), hits_(std::move(hits)), confirmed_(std::move(confirmed)) {}

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext & /*context*/, int32_t /*doc*/,
                                              const IndexSearcher & /*searcher*/) const override {
        return Explanation::no_match_no_details("the fixed weight does not explain");
    }

    [[nodiscard]] const Query &get_query() const override;

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext & /*context*/,
                                                                         const IndexSearcher & /*searcher*/) override {
        if (hits_.empty()) {
            return std::unique_ptr<ScorerSupplier>{};
        }
        return std::unique_ptr<ScorerSupplier>(new FixedScorerSupplier(hits_, confirmed_));
    }

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext & /*context*/) const override { return true; }

private:
    const FixedQuery &query_;
    std::vector<Hit> hits_;
    std::vector<int32_t> confirmed_;
};

class FixedQuery final : public Query {
public:
    explicit FixedQuery(std::vector<Hit> hits, std::vector<int32_t> confirmed = {})
            : hits_(std::move(hits)), confirmed_(std::move(confirmed)) {}

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher & /*searcher*/,
                                                                ScoreMode /*score_mode*/,
                                                                float /*boost*/) const override {
        return std::unique_ptr<Weight>(new FixedWeight(*this, hits_, confirmed_));
    }

    [[nodiscard]] VoidResult visit(QueryVisitor & /*visitor*/) const override { return {}; }

    [[nodiscard]] Result<std::string> to_string(std::string_view /*field*/) const override { return "FixedQuery"; }

    [[nodiscard]] bool equals(const Query &other) const override { return this == &other; }

    [[nodiscard]] size_t hash() const override { return 7; }

private:
    std::vector<Hit> hits_;
    std::vector<int32_t> confirmed_;
};

const Query &FixedWeight::get_query() const { return query_; }

/// A leaf reader with no postings at all: the weight only needs its identity
/// and its document count.
class EmptyLeafReader final : public LeafReader {
public:
    explicit EmptyLeafReader(int32_t max_doc) : max_doc_(max_doc) {}

    [[nodiscard]] Result<int32_t> max_doc() const override { return max_doc_; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return max_doc_; }

    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override {
        return std::unique_ptr<StoredFields>{};
    }
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override {
        return std::unique_ptr<TermVectors>{};
    }
    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view /*field*/) override {
        return std::unique_ptr<Terms>{};
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
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norm_values(std::string_view /*field*/) override {
        return std::unique_ptr<NumericDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<DocValuesSkipper>> get_doc_values_skipper(
            std::string_view /*field*/) override {
        return std::unique_ptr<DocValuesSkipper>{};
    }

    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return &field_infos_; }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return static_cast<const Bits *>(nullptr); }

private:
    int32_t max_doc_;
    FieldInfos field_infos_{FieldInfos::none()};
};

IndexSearcher make_searcher(int32_t max_doc) {
    return IndexSearcher(std::make_shared<EmptyLeafReader>(max_doc));
}

// ------------------------------------------------------- TermWeight::matches

/// One posting of the fake field, with the positions and offsets a matches
/// iterator needs.
struct RichPosting {
    int32_t doc;
    std::vector<int32_t> positions;
    /// Two entries per position: start and end offset.
    std::vector<int32_t> offsets;
};

class RichPostingsEnum final : public PostingsEnum {
public:
    explicit RichPostingsEnum(std::vector<RichPosting> postings) : postings_(std::move(postings)) {}

    [[nodiscard]] int32_t doc_id() const override {
        if (index_ < 0) {
            return -1;
        }
        return index_ < size() ? postings_[static_cast<size_t>(index_)].doc : NO_MORE_DOCS;
    }

    [[nodiscard]] Result<int32_t> next_doc() override {
        ++index_;
        position_ = -1;
        return doc_id();
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        if (index_ >= 0 && doc_id() >= target) {
            return doc_id();
        }
        do {
            ++index_;
        } while (doc_id() < target && doc_id() != NO_MORE_DOCS);
        position_ = -1;
        return doc_id();
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(size()); }

    [[nodiscard]] Result<int32_t> freq() override {
        if (index_ < 0 || index_ >= size()) {
            return err(LuceneError::illegal_state("the postings are not positioned"));
        }
        return static_cast<int32_t>(postings_[static_cast<size_t>(index_)].positions.size());
    }

    [[nodiscard]] Result<int32_t> next_position() override {
        if (index_ < 0 || index_ >= size()) {
            return err(LuceneError::illegal_state("the postings are not positioned"));
        }
        ++position_;
        const auto &positions = postings_[static_cast<size_t>(index_)].positions;
        if (position_ >= static_cast<int32_t>(positions.size())) {
            return err(LuceneError::illegal_state("the postings are exhausted"));
        }
        return positions[static_cast<size_t>(position_)];
    }

    [[nodiscard]] Result<int32_t> start_offset() const override { return offset(0); }
    [[nodiscard]] Result<int32_t> end_offset() const override { return offset(1); }

    [[nodiscard]] Result<std::optional<std::span<const uint8_t>>> get_payload() const override {
        return std::nullopt;
    }

private:
    [[nodiscard]] size_t size() const noexcept { return postings_.size(); }

    [[nodiscard]] Result<int32_t> offset(size_t which) const {
        if (index_ < 0 || index_ >= size() || position_ < 0) {
            return err(LuceneError::illegal_state("the postings are not positioned"));
        }
        const auto &offsets = postings_[static_cast<size_t>(index_)].offsets;
        const size_t at = static_cast<size_t>(position_) * 2 + which;
        if (at >= offsets.size()) {
            return err(LuceneError::illegal_state("the posting has no offset"));
        }
        return offsets[at];
    }

    std::vector<RichPosting> postings_;
    int32_t index_{-1};
    int32_t position_{-1};
};

class RichTermsEnum final : public TermsEnum {
public:
    explicit RichTermsEnum(std::map<std::string, std::vector<RichPosting>> terms) : terms_(std::move(terms)) {}

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

    [[nodiscard]] Result<SeekStatus> seek_ceil(std::span<const uint8_t> target) override {
        const std::string text(reinterpret_cast<const char *>(target.data()), target.size());
        for (size_t i = 0; i < terms_.size(); ++i) {
            auto it = terms_.begin();
            std::advance(it, static_cast<std::ptrdiff_t>(i));
            if (it->first < text) {
                continue;
            }
            pos_ = i;
            return it->first == text ? SeekStatus::Found : SeekStatus::NotFound;
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

    [[nodiscard]] Result<const BytesRef *> term() const override {
        if (!pos_.has_value()) {
            return err(LuceneError::illegal_state("the terms enum is not positioned"));
        }
        return &term_at(pos_.value());
    }

    [[nodiscard]] Result<int64_t> ord() const override { return static_cast<int64_t>(pos_.value_or(0)); }

    [[nodiscard]] Result<int32_t> doc_freq() override {
        if (!pos_.has_value()) {
            return err(LuceneError::illegal_state("the terms enum is not positioned"));
        }
        return static_cast<int32_t>(postings_at(pos_.value()).size());
    }

    [[nodiscard]] Result<int64_t> total_term_freq() override {
        if (!pos_.has_value()) {
            return err(LuceneError::illegal_state("the terms enum is not positioned"));
        }
        int64_t total = 0;
        for (const auto &posting : postings_at(pos_.value())) {
            total += static_cast<int64_t>(posting.positions.size());
        }
        return total;
    }

    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>> postings_with_flags(std::unique_ptr<PostingsEnum> /*reuse*/,
                                                                           int32_t /*flags*/) override {
        if (!pos_.has_value()) {
            return err(LuceneError::illegal_state("the terms enum is not positioned"));
        }
        return std::unique_ptr<PostingsEnum>(std::make_unique<RichPostingsEnum>(postings_at(pos_.value())));
    }

    [[nodiscard]] Result<std::unique_ptr<ImpactsEnum>> impacts(int32_t /*flags*/) override {
        return err(LuceneError::unsupported_operation("the fake terms have no impacts"));
    }

    [[nodiscard]] Result<AttributeSource *> attributes() override {
        return err(LuceneError::unsupported_operation("the fake terms have no attributes"));
    }

    [[nodiscard]] Result<AttributeSource *> attributes_mut() override {
        return err(LuceneError::unsupported_operation("the fake terms have no attributes"));
    }

    [[nodiscard]] Result<TermStateEnum> term_state() override {
        if (!pos_.has_value()) {
            return err(LuceneError::illegal_state("the terms enum is not positioned"));
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

    [[nodiscard]] const std::vector<RichPosting> &postings_at(size_t index) const {
        auto it = terms_.begin();
        std::advance(it, static_cast<std::ptrdiff_t>(index));
        return it->second;
    }

    std::map<std::string, std::vector<RichPosting>> terms_;
    std::optional<size_t> pos_{};
    mutable BytesRef cache_{};
};

class RichTerms final : public Terms {
public:
    explicit RichTerms(std::map<std::string, std::vector<RichPosting>> terms) : terms_(std::move(terms)) {}

    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> iterator() const override {
        return std::unique_ptr<TermsEnum>(std::make_unique<RichTermsEnum>(terms_));
    }

    [[nodiscard]] Result<int64_t> size() const override { return static_cast<int64_t>(terms_.size()); }

    [[nodiscard]] Result<int64_t> sum_total_term_freq() const override {
        int64_t total = 0;
        for (const auto &[text, postings] : terms_) {
            for (const auto &posting : postings) {
                total += static_cast<int64_t>(posting.positions.size());
            }
        }
        return total;
    }

    [[nodiscard]] Result<int64_t> sum_doc_freq() const override {
        int64_t total = 0;
        for (const auto &[text, postings] : terms_) {
            total += static_cast<int64_t>(postings.size());
        }
        return total;
    }

    [[nodiscard]] Result<int32_t> doc_count() const override {
        int32_t count = 0;
        for (const auto &[text, postings] : terms_) {
            count += static_cast<int32_t>(postings.size());
        }
        return count;
    }

    [[nodiscard]] bool has_freqs() const override { return true; }
    [[nodiscard]] bool has_offsets() const override { return true; }
    [[nodiscard]] bool has_positions() const override { return true; }
    [[nodiscard]] bool has_payloads() const override { return false; }

private:
    std::map<std::string, std::vector<RichPosting>> terms_;
};

/// A leaf reader whose only field has positions and offsets.
class RichLeafReader final : public LeafReader {
public:
    RichLeafReader(int32_t max_doc, std::map<std::string, std::vector<RichPosting>> terms)
            : max_doc_(max_doc), terms_(std::move(terms)) {}

    [[nodiscard]] Result<int32_t> max_doc() const override { return max_doc_; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return max_doc_; }

    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override {
        return std::unique_ptr<StoredFields>{};
    }
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override {
        return std::unique_ptr<TermVectors>{};
    }

    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view field) override {
        if (field != kField) {
            return std::unique_ptr<Terms>{};
        }
        return std::unique_ptr<Terms>(std::make_unique<RichTerms>(terms_));
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
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norm_values(std::string_view /*field*/) override {
        return std::unique_ptr<NumericDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<DocValuesSkipper>> get_doc_values_skipper(
            std::string_view /*field*/) override {
        return std::unique_ptr<DocValuesSkipper>{};
    }

    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return &field_infos_; }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return static_cast<const Bits *>(nullptr); }

private:
    int32_t max_doc_;
    std::map<std::string, std::vector<RichPosting>> terms_;
    FieldInfos field_infos_{FieldInfos::none()};
};

/// "foo" is in doc 1 at positions 3 and 7 and in doc 3 at position 1.
std::shared_ptr<IndexReader> make_rich_reader() {
    std::map<std::string, std::vector<RichPosting>> terms;
    terms["foo"] = {RichPosting{1, {3, 7}, {10, 13, 20, 23}}, RichPosting{3, {1}, {5, 8}}};
    terms["other"] = {RichPosting{2, {9}, {30, 33}}};
    return std::make_shared<RichLeafReader>(4, std::move(terms));
}

/// A similarity that never scores: the matches tests only need positions.
class SilentScorer final : public SimScorer {
public:
    [[nodiscard]] float score(float /*freq*/, int64_t /*norm*/) const override { return 0.0F; }
};

class SilentSimilarity final : public Similarity {
public:
    [[nodiscard]] Result<std::unique_ptr<SimScorer>> scorer(
            float /*boost*/, const CollectionStatistics & /*collection_stats*/,
            std::span<const TermStatistics> /*term_stats*/) const override {
        return std::unique_ptr<SimScorer>(std::make_unique<SilentScorer>());
    }

    [[nodiscard]] std::string name() const override { return "SilentSimilarity"; }
};

/// The postings of one term on one document, with positions and offsets.
class PositionedPostings final : public PostingsEnum {
public:
    PositionedPostings(int32_t doc, std::vector<int32_t> positions, std::vector<std::pair<int32_t, int32_t>> offsets)
            : doc_(doc), positions_(std::move(positions)), offsets_(std::move(offsets)) {}

    [[nodiscard]] int32_t doc_id() const override { return positioned_ ? doc_ : -1; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        positioned_ = true;
        return doc_;
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        positioned_ = true;
        return target <= doc_ ? doc_ : NO_MORE_DOCS;
    }

    [[nodiscard]] Result<int64_t> cost() const override { return 1; }

    [[nodiscard]] Result<int32_t> freq() override { return static_cast<int32_t>(positions_.size()); }

    [[nodiscard]] Result<int32_t> next_position() override {
        ++position_;
        if (position_ >= static_cast<int32_t>(positions_.size())) {
            return err(LuceneError::illegal_state("no more positions"));
        }
        return positions_[static_cast<size_t>(position_)];
    }

    [[nodiscard]] Result<int32_t> start_offset() const override {
        if (position_ < 0 || position_ >= static_cast<int32_t>(offsets_.size())) {
            return -1;
        }
        return offsets_[static_cast<size_t>(position_)].first;
    }

    [[nodiscard]] Result<int32_t> end_offset() const override {
        if (position_ < 0 || position_ >= static_cast<int32_t>(offsets_.size())) {
            return -1;
        }
        return offsets_[static_cast<size_t>(position_)].second;
    }

    [[nodiscard]] Result<std::optional<std::span<const uint8_t>>> get_payload() const override {
        return std::optional<std::span<const uint8_t>>{};
    }

private:
    int32_t doc_{0};
    std::vector<int32_t> positions_;
    std::vector<std::pair<int32_t, int32_t>> offsets_;
    bool positioned_{false};
    int32_t position_{-1};
};

/// A query that only serves as the identity of a match.
class MarkerQuery final : public Query {
public:
    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher & /*searcher*/,
                                                               ScoreMode /*score_mode*/,
                                                               float /*boost*/) const override {
        return err(LuceneError::unsupported_operation("MarkerQuery cannot be searched"));
    }
    [[nodiscard]] VoidResult visit(QueryVisitor & /*visitor*/) const override { return {}; }
    [[nodiscard]] Result<std::string> to_string(std::string_view /*field*/) const override {
        return std::string("MarkerQuery");
    }
    [[nodiscard]] bool equals(const Query &other) const override {
        return dynamic_cast<const MarkerQuery *>(&other) != nullptr;
    }
    [[nodiscard]] size_t hash() const override { return 3; }
};

std::unique_ptr<TermMatchesIterator> make_iterator(std::shared_ptr<Query> query) {
    auto postings = std::make_unique<PositionedPostings>(5, std::vector<int32_t>{1, 3, 7},
                                                         std::vector<std::pair<int32_t, int32_t>>{{12, 15}, {30, 33},
                                                                                                  {70, 73}});
    auto iterator = TermMatchesIterator::create(std::move(postings), std::move(query));
    EXPECT_TRUE(iterator.has_value()) << iterator.error().to_string();
    return iterator.has_value() ? std::move(iterator.value()) : nullptr;
}

} // namespace

TEST(MatchesTest, DefaultWeightMatchesReportsNoTerms) {
    IndexSearcher searcher = make_searcher(4);
    FixedQuery query({Hit{1, 1.0F}});

    auto weight = query.create_weight(searcher, ScoreMode::CompleteNoScores, 1.0F);
    ASSERT_TRUE(weight.has_value());
    auto leaves = searcher.leaves();
    ASSERT_TRUE(leaves.has_value());

    auto matching = (*weight)->matches(leaves.value()[0], 1, searcher);
    ASSERT_TRUE(matching.has_value()) << matching.error().to_string();
    ASSERT_TRUE(matching.value() != nullptr);
    EXPECT_EQ(dynamic_cast<const MatchWithNoTerms *>(matching.value().get()) != nullptr, true);
    EXPECT_TRUE(matching.value()->fields().empty());
    EXPECT_TRUE(matching.value()->get_matches("body").value() == nullptr);

    auto missing = (*weight)->matches(leaves.value()[0], 0, searcher);
    ASSERT_TRUE(missing.has_value()) << missing.error().to_string();
    EXPECT_TRUE(missing.value() == nullptr);
}

TEST(MatchesTest, WeightMatchesWithoutAScorerIsNoMatch) {
    IndexSearcher searcher = make_searcher(4);
    FixedQuery query({});

    auto weight = query.create_weight(searcher, ScoreMode::CompleteNoScores, 1.0F);
    ASSERT_TRUE(weight.has_value());
    auto leaves = searcher.leaves();
    ASSERT_TRUE(leaves.has_value());

    auto matches = (*weight)->matches(leaves.value()[0], 0, searcher);
    ASSERT_TRUE(matches.has_value()) << matches.error().to_string();
    EXPECT_TRUE(matches.value() == nullptr);
}

TEST(MatchesTest, WeightMatchesConfirmsTwoPhaseCandidates) {
    IndexSearcher searcher = make_searcher(4);
    // The approximation visits every document, but only doc 2 matches.
    FixedQuery query({Hit{0, 1.0F}, Hit{1, 1.0F}, Hit{2, 1.0F}, Hit{3, 1.0F}}, {2});

    auto weight = query.create_weight(searcher, ScoreMode::CompleteNoScores, 1.0F);
    ASSERT_TRUE(weight.has_value());
    auto leaves = searcher.leaves();
    ASSERT_TRUE(leaves.has_value());

    auto confirmed = (*weight)->matches(leaves.value()[0], 2, searcher);
    ASSERT_TRUE(confirmed.has_value()) << confirmed.error().to_string();
    EXPECT_TRUE(confirmed.value() != nullptr);

    auto rejected = (*weight)->matches(leaves.value()[0], 1, searcher);
    ASSERT_TRUE(rejected.has_value()) << rejected.error().to_string();
    EXPECT_TRUE(rejected.value() == nullptr);
}

TEST(MatchesTest, TermWeightMatchesReportsPositionsAndOffsets) {
    auto reader = make_rich_reader();
    IndexSearcher searcher(reader, std::make_shared<SilentSimilarity>());
    const TermQuery query(Term::from_text(std::string(kField), "foo"));

    auto weight = query.create_weight(searcher, ScoreMode::CompleteNoScores, 1.0F);
    ASSERT_TRUE(weight.has_value());
    auto leaves = searcher.leaves();
    ASSERT_TRUE(leaves.has_value());

    auto matches = (*weight)->matches(leaves.value()[0], 1, searcher);
    ASSERT_TRUE(matches.has_value()) << matches.error().to_string();
    ASSERT_TRUE(matches.value() != nullptr);
    EXPECT_EQ(matches.value()->fields(), (std::vector<std::string>{std::string(kField)}));

    auto iterator = matches.value()->get_matches(kField);
    ASSERT_TRUE(iterator.has_value()) << iterator.error().to_string();
    ASSERT_TRUE(iterator.value() != nullptr);

    ASSERT_TRUE(iterator.value()->next().value());
    EXPECT_EQ(iterator.value()->start_position().value(), 3);
    EXPECT_EQ(iterator.value()->end_position().value(), 3);
    EXPECT_EQ(iterator.value()->start_offset().value(), 10);
    EXPECT_EQ(iterator.value()->end_offset().value(), 13);

    ASSERT_TRUE(iterator.value()->next().value());
    EXPECT_EQ(iterator.value()->start_position().value(), 7);
    EXPECT_EQ(iterator.value()->start_offset().value(), 20);
    EXPECT_EQ(iterator.value()->end_offset().value(), 23);

    EXPECT_FALSE(iterator.value()->next().value());

    // Another field has no matches.
    auto other_field = matches.value()->get_matches("title");
    ASSERT_TRUE(other_field.has_value());
    EXPECT_TRUE(other_field.value() == nullptr);
}

TEST(MatchesTest, TermWeightMatchesRejectsDocumentsWithoutTheTerm) {
    auto reader = make_rich_reader();
    IndexSearcher searcher(reader, std::make_shared<SilentSimilarity>());
    const TermQuery query(Term::from_text(std::string(kField), "foo"));

    auto weight = query.create_weight(searcher, ScoreMode::CompleteNoScores, 1.0F);
    ASSERT_TRUE(weight.has_value());
    auto leaves = searcher.leaves();
    ASSERT_TRUE(leaves.has_value());

    auto absent_doc = (*weight)->matches(leaves.value()[0], 0, searcher);
    ASSERT_TRUE(absent_doc.has_value()) << absent_doc.error().to_string();
    EXPECT_TRUE(absent_doc.value() == nullptr);

    const TermQuery unknown(Term::from_text(std::string(kField), "nope"));
    auto unknown_weight = unknown.create_weight(searcher, ScoreMode::CompleteNoScores, 1.0F);
    ASSERT_TRUE(unknown_weight.has_value());
    auto unknown_matches = (*unknown_weight)->matches(leaves.value()[0], 1, searcher);
    ASSERT_TRUE(unknown_matches.has_value()) << unknown_matches.error().to_string();
    EXPECT_TRUE(unknown_matches.value() == nullptr);
}

TEST(MatchesTest, FromSubMatchesCollapsesSingleMatches) {
    EXPECT_TRUE(from_sub_matches({}) == nullptr);

    std::vector<std::unique_ptr<Matches>> only_no_terms;
    only_no_terms.push_back(match_with_no_terms());
    auto collapsed = from_sub_matches(std::move(only_no_terms));
    ASSERT_TRUE(collapsed != nullptr);
    EXPECT_TRUE(dynamic_cast<const MatchWithNoTerms *>(collapsed.get()) != nullptr);

    std::vector<std::unique_ptr<Matches>> one;
    one.push_back(std::make_unique<FakeMatches>(std::string(kField), std::vector<MatchSpan>{{1, 1, 0, 1}}));
    auto single = from_sub_matches(std::move(one));
    ASSERT_TRUE(single != nullptr);
    EXPECT_TRUE(dynamic_cast<const FakeMatches *>(single.get()) != nullptr);
}

TEST(MatchesTest, FromSubMatchesCombinesSeveralMatches) {
    std::vector<std::unique_ptr<Matches>> sub;
    sub.push_back(std::make_unique<FakeMatches>(std::string(kField), std::vector<MatchSpan>{{1, 1, 0, 1}}));
    sub.push_back(std::make_unique<FakeMatches>(std::string(kField), std::vector<MatchSpan>{{4, 4, 5, 6}}));
    auto combined = from_sub_matches(std::move(sub));
    ASSERT_TRUE(combined != nullptr);
    const auto *match = dynamic_cast<const CombinedMatch *>(combined.get());
    ASSERT_TRUE(match != nullptr);
    EXPECT_EQ(match->sub_matches().size(), 2);

    auto iterator = combined->get_matches(kField);
    ASSERT_TRUE(iterator.has_value()) << iterator.error().to_string();
    ASSERT_TRUE(iterator.value() != nullptr);
    EXPECT_EQ(drain_positions(*iterator.value()), (std::vector<int32_t>{1, 4}));
}

TEST(MatchesTest, CombinedMatchUnionsTheFields) {
    std::vector<std::unique_ptr<Matches>> sub;
    sub.push_back(std::make_unique<FakeMatches>(std::string(kField), std::vector<MatchSpan>{{1, 1, 0, 1}}));
    sub.push_back(std::make_unique<FakeMatches>("title", std::vector<MatchSpan>{{2, 2, 3, 4}}));
    sub.push_back(std::make_unique<FakeMatches>(std::string(kField), std::vector<MatchSpan>{{5, 5, 6, 7}}));
    CombinedMatch combined(std::move(sub));

    EXPECT_EQ(combined.fields(), (std::vector<std::string>{std::string(kField), "title"}));

    auto body = combined.get_matches(kField);
    ASSERT_TRUE(body.has_value());
    ASSERT_TRUE(body.value() != nullptr);
    EXPECT_EQ(drain_positions(*body.value()), (std::vector<int32_t>{1, 5}));

    auto title = combined.get_matches("title");
    ASSERT_TRUE(title.has_value());
    ASSERT_TRUE(title.value() != nullptr);
    EXPECT_EQ(drain_positions(*title.value()), (std::vector<int32_t>{2}));
}

TEST(MatchesTest, DisjunctionMatchesIteratorOrdersByStartThenEndPosition) {
    std::vector<std::unique_ptr<MatchesIterator>> sub;
    sub.push_back(std::make_unique<FakeMatchesIterator>(std::vector<MatchSpan>{{3, 7, 10, 14}, {9, 9, 20, 21}}));
    sub.push_back(std::make_unique<FakeMatchesIterator>(std::vector<MatchSpan>{{3, 5, 8, 11}}));

    auto iterator = DisjunctionMatchesIterator::from_sub_iterators(std::move(sub));
    ASSERT_TRUE(iterator.has_value()) << iterator.error().to_string();
    ASSERT_TRUE(iterator.value() != nullptr);

    // A prefix sorts first, then the longer match at the same start.
    EXPECT_EQ(drain_positions(*iterator.value()), (std::vector<int32_t>{3, 3, 9}));
}

TEST(MatchesTest, DisjunctionMatchesIteratorDropsExhaustedSubIterators) {
    std::vector<std::unique_ptr<MatchesIterator>> sub;
    sub.push_back(std::make_unique<FakeMatchesIterator>(std::vector<MatchSpan>{{1, 1, 0, 1}}));
    sub.push_back(std::make_unique<FakeMatchesIterator>(std::vector<MatchSpan>{{2, 2, 2, 3}, {4, 4, 4, 5}}));
    sub.push_back(std::make_unique<FakeMatchesIterator>(std::vector<MatchSpan>{}));

    auto iterator = DisjunctionMatchesIterator::from_sub_iterators(std::move(sub));
    ASSERT_TRUE(iterator.has_value());
    ASSERT_TRUE(iterator.value() != nullptr);
    EXPECT_EQ(drain_positions(*iterator.value()), (std::vector<int32_t>{1, 2, 4}));
}

TEST(MatchesTest, DisjunctionMatchesIteratorPassesThroughSingleIterator) {
    auto empty = DisjunctionMatchesIterator::from_sub_iterators({});
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(empty.value() == nullptr);

    std::vector<std::unique_ptr<MatchesIterator>> one;
    one.push_back(std::make_unique<FakeMatchesIterator>(std::vector<MatchSpan>{{6, 6, 7, 8}}));
    auto single = DisjunctionMatchesIterator::from_sub_iterators(std::move(one));
    ASSERT_TRUE(single.has_value());
    ASSERT_TRUE(single.value() != nullptr);
    EXPECT_EQ(drain_positions(*single.value()), (std::vector<int32_t>{6}));

    std::vector<std::unique_ptr<MatchesIterator>> no_matches;
    no_matches.push_back(std::make_unique<FakeMatchesIterator>(std::vector<MatchSpan>{}));
    no_matches.push_back(std::make_unique<FakeMatchesIterator>(std::vector<MatchSpan>{}));
    auto none = DisjunctionMatchesIterator::from_sub_iterators(std::move(no_matches));
    ASSERT_TRUE(none.has_value());
    EXPECT_TRUE(none.value() == nullptr);
}

TEST(MatchesTest, ForFieldCachesTheFirstIterator) {
    int32_t calls = 0;
    auto supplier = [&calls]() -> Result<std::unique_ptr<MatchesIterator>> {
        ++calls;
        return std::unique_ptr<MatchesIterator>(std::make_unique<FakeMatchesIterator>(std::vector<MatchSpan>{{1, 1, 0, 1}}));
    };

    auto matches = for_field(std::string(kField), supplier);
    ASSERT_TRUE(matches.has_value()) << matches.error().to_string();
    ASSERT_TRUE(matches.value() != nullptr);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(matches.value()->fields(), (std::vector<std::string>{std::string(kField)}));

    // The first iterator is the one built eagerly.
    auto first = matches.value()->get_matches(kField);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(first.value() != nullptr);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(drain_positions(*first.value()), (std::vector<int32_t>{1}));

    // The next call rebuilds through the supplier.
    auto second = matches.value()->get_matches(kField);
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(second.value() != nullptr);
    EXPECT_EQ(calls, 2);

    auto other_field = matches.value()->get_matches("title");
    ASSERT_TRUE(other_field.has_value());
    EXPECT_TRUE(other_field.value() == nullptr);
}

TEST(MatchesTest, ForFieldWithoutAnIteratorIsNoMatch) {
    auto matches = for_field(std::string(kField), []() -> Result<std::unique_ptr<MatchesIterator>> {
        return std::unique_ptr<MatchesIterator>{};
    });
    ASSERT_TRUE(matches.has_value()) << matches.error().to_string();
    EXPECT_TRUE(matches.value() == nullptr);
}

TEST(MatchesTest, TermIteratorWalksEveryPosition) {
    auto iterator = make_iterator(std::make_shared<MarkerQuery>());
    ASSERT_NE(iterator, nullptr);

    std::vector<int32_t> positions;
    std::vector<int32_t> starts;
    std::vector<int32_t> ends;
    while (true) {
        auto has_next = iterator->next();
        ASSERT_TRUE(has_next.has_value()) << has_next.error().to_string();
        if (!has_next.value()) {
            break;
        }
        positions.push_back(iterator->start_position().value());
        EXPECT_EQ(iterator->end_position().value(), iterator->start_position().value());
        starts.push_back(iterator->start_offset().value());
        ends.push_back(iterator->end_offset().value());
    }

    EXPECT_EQ(positions, (std::vector<int32_t>{1, 3, 7}));
    EXPECT_EQ(starts, (std::vector<int32_t>{12, 30, 70}));
    EXPECT_EQ(ends, (std::vector<int32_t>{15, 33, 73}));
}

TEST(MatchesTest, TermIteratorIsExhaustedAfterTheLastPosition) {
    auto iterator = make_iterator(std::make_shared<MarkerQuery>());
    ASSERT_NE(iterator, nullptr);
    for (int32_t i = 0; i < 3; ++i) {
        auto has_next = iterator->next();
        ASSERT_TRUE(has_next.has_value());
        EXPECT_TRUE(has_next.value());
    }
    auto exhausted = iterator->next();
    ASSERT_TRUE(exhausted.has_value());
    EXPECT_FALSE(exhausted.value());
}

TEST(MatchesTest, TermIteratorHasNoSubMatchesButKnowsItsQuery) {
    auto query = std::make_shared<MarkerQuery>();
    auto iterator = make_iterator(query);
    ASSERT_NE(iterator, nullptr);

    auto has_next = iterator->next();
    ASSERT_TRUE(has_next.has_value());
    ASSERT_TRUE(has_next.value());

    auto sub_matches = iterator->get_sub_matches();
    ASSERT_TRUE(sub_matches.has_value());
    EXPECT_EQ(sub_matches.value(), nullptr);

    auto reported = iterator->get_query();
    ASSERT_TRUE(reported.has_value());
    EXPECT_EQ(reported.value(), query);
}

TEST(MatchesTest, TermIteratorNeedsPostings) {
    auto iterator = TermMatchesIterator::create(nullptr, std::make_shared<MarkerQuery>());
    ASSERT_FALSE(iterator.has_value());
    EXPECT_EQ(iterator.error().kind(), ErrorKind::kIllegalArgument);
}
