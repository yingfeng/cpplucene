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
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

module lucene_core:ut.term_states;

import lucene_core;

using namespace lucene;

namespace {

/// Per-term statistics of the fake field.
struct TermStat {
    int32_t doc_freq{1};
    int64_t total_term_freq{1};
};

/// A [`TermsEnum`] over an ordered map of term text to statistics.
class StatTermsEnum final : public TermsEnum {
public:
    StatTermsEnum(std::map<std::string, TermStat> terms, int32_t leaf_ord)
        : terms_(std::move(terms)), leaf_ord_(leaf_ord) {}

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
        return err(LuceneError::unsupported_operation("StatTermsEnum has no attributes"));
    }
    [[nodiscard]] Result<AttributeSource *> attributes_mut() override {
        return err(LuceneError::unsupported_operation("StatTermsEnum has no attributes"));
    }

    [[nodiscard]] Result<SeekStatus> seek_ceil(std::span<const uint8_t> term) override {
        auto found = find(term);
        if (!found.has_value()) {
            return std::unexpected(std::move(found.error()));
        }
        if (found.value().second == Match::Exact) {
            pos_ = found.value().first;
            return SeekStatus::Found;
        }
        if (found.value().second == Match::After) {
            pos_ = found.value().first;
            return SeekStatus::NotFound;
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

    /// The first phase only remembers that a seek is required, like a prefetch.
    [[nodiscard]] Result<bool> prepare_seek_exact(std::span<const uint8_t> term) override {
        prepared_ = BytesRef::from_string(utf8(term));
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

    [[nodiscard]] VoidResult seek_exact_with_state(std::span<const uint8_t> /*term*/,
                                                   const TermStateEnum & /*state*/) override {
        return err(LuceneError::unsupported_operation("StatTermsEnum has no term state seek"));
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
        return stat_at(pos_.value()).doc_freq;
    }

    [[nodiscard]] Result<int64_t> total_term_freq() override {
        if (!pos_.has_value()) {
            return err(LuceneError::illegal_state("unpositioned"));
        }
        return stat_at(pos_.value()).total_term_freq;
    }

    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>> postings_with_flags(std::unique_ptr<PostingsEnum> /*reuse*/,
                                                                          int32_t /*flags*/) override {
        return err(LuceneError::unsupported_operation("StatTermsEnum has no postings"));
    }

    [[nodiscard]] Result<std::unique_ptr<ImpactsEnum>> impacts(int32_t /*flags*/) override {
        return err(LuceneError::unsupported_operation("StatTermsEnum has no impacts"));
    }

    /// The state records the leaf and the term ordinal, so a test can tell which
    /// leaf resolved it.
    [[nodiscard]] Result<TermStateEnum> term_state() override {
        if (!pos_.has_value()) {
            return err(LuceneError::illegal_state("unpositioned"));
        }
        return TermStateEnum(OrdTermState(static_cast<int64_t>(leaf_ord_) * 1000 +
                                         static_cast<int64_t>(pos_.value())));
    }

private:
    enum class Match : uint8_t { Exact, After, None };

    static std::string utf8(std::span<const uint8_t> bytes) {
        return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    }

    [[nodiscard]] const BytesRef &term_at(size_t index) const {
        auto it = terms_.begin();
        std::advance(it, static_cast<std::ptrdiff_t>(index));
        // The keys are stable, but `BytesRef` is what the interface hands out.
        cache_ = BytesRef::from_string(it->first);
        return cache_;
    }

    [[nodiscard]] TermStat stat_at(size_t index) const {
        auto it = terms_.begin();
        std::advance(it, static_cast<std::ptrdiff_t>(index));
        return it->second;
    }

    [[nodiscard]] Result<std::pair<size_t, Match>> find(std::span<const uint8_t> term) const {
        size_t index = 0;
        for (const auto &[text, stat] : terms_) {
            const bool term_before = std::lexicographical_compare(term.begin(), term.end(), text.begin(), text.end());
            if (term_before) {
                // The term sorts before this entry: this entry is its ceiling.
                return std::pair<size_t, Match>{index, Match::After};
            }
            const bool text_before = std::lexicographical_compare(text.begin(), text.end(), term.begin(), term.end());
            if (!text_before) {
                return std::pair<size_t, Match>{index, Match::Exact};
            }
            ++index;
        }
        return std::pair<size_t, Match>{0, Match::None};
    }

    std::map<std::string, TermStat> terms_;
    int32_t leaf_ord_;
    std::optional<size_t> pos_{};
    std::optional<BytesRef> prepared_{};
    mutable BytesRef cache_{};
};

/// [`Terms`] over the fake field of one leaf.
class StatTerms final : public Terms {
public:
    StatTerms(std::map<std::string, TermStat> terms, int32_t leaf_ord)
        : terms_(std::move(terms)), leaf_ord_(leaf_ord) {}

    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> iterator() const override {
        return std::unique_ptr<TermsEnum>(std::make_unique<StatTermsEnum>(terms_, leaf_ord_));
    }

    [[nodiscard]] Result<int64_t> size() const override { return static_cast<int64_t>(terms_.size()); }

    [[nodiscard]] Result<int64_t> sum_total_term_freq() const override {
        int64_t sum = 0;
        for (const auto &[text, stat] : terms_) {
            sum += stat.total_term_freq;
        }
        return sum;
    }

    [[nodiscard]] Result<int64_t> sum_doc_freq() const override {
        int64_t sum = 0;
        for (const auto &[text, stat] : terms_) {
            sum += stat.doc_freq;
        }
        return sum;
    }

    [[nodiscard]] Result<int32_t> doc_count() const override {
        int32_t count = 0;
        for (const auto &[text, stat] : terms_) {
            count += stat.doc_freq;
        }
        return count;
    }

    [[nodiscard]] bool has_freqs() const override { return true; }
    [[nodiscard]] bool has_offsets() const override { return false; }
    [[nodiscard]] bool has_positions() const override { return false; }
    [[nodiscard]] bool has_payloads() const override { return false; }

private:
    std::map<std::string, TermStat> terms_;
    int32_t leaf_ord_;
};

/// A [`LeafReader`] with one fake terms field and configurable doc counts.
class FakeLeafReader final : public LeafReader {
public:
    FakeLeafReader(std::string field, std::map<std::string, TermStat> terms, int32_t max_doc,
                   int32_t num_docs, int32_t ord)
        : field_(std::move(field)), terms_(std::move(terms)), max_doc_(max_doc), num_docs_(num_docs), ord_(ord) {}

    [[nodiscard]] Result<int32_t> max_doc() const override { return max_doc_; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return num_docs_; }

    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override { return nullptr; }
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override { return nullptr; }

    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view field) override {
        if (field != field_) {
            return std::unique_ptr<Terms>{};
        }
        return std::unique_ptr<Terms>(std::make_unique<StatTerms>(terms_, ord_));
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
    [[nodiscard]] Result<std::unique_ptr<DocValuesSkipper>> get_doc_values_skipper(std::string_view /*field*/) override {
        return nullptr;
    }

    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return &field_infos_; }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return static_cast<const Bits *>(nullptr); }

private:
    std::string field_;
    std::map<std::string, TermStat> terms_;
    int32_t max_doc_;
    int32_t num_docs_;
    int32_t ord_;
    FieldInfos field_infos_{FieldInfos::none()};
};

/// A composite [`IndexReader`]: a fixed list of leaves, and document/term
/// aggregates over them.
class FakeCompositeReader final : public IndexReader {
public:
    explicit FakeCompositeReader(std::vector<std::unique_ptr<LeafReader>> readers) {
        int32_t doc_base = 0;
        int32_t max_doc = 0;
        for (size_t ord = 0; ord < readers.size(); ++ord) {
            auto max = readers[ord]->max_doc();
            auto num = readers[ord]->num_docs();
            max_doc += max.value();
            num_docs_ += num.value();
            auto leaf_max = max.value();
            leaves_.emplace_back(readers[ord].get(), static_cast<int32_t>(ord), doc_base,
                                 TopParentMeta{static_cast<int32_t>(readers.size()), max_doc, identity_});
            doc_base += leaf_max;
            readers_.push_back(std::move(readers[ord]));
        }
        max_doc_ = max_doc;
        // The top-level metadata must agree on the total document count.
        for (auto &leaf : leaves_) {
            leaf = LeafReaderContext(leaf.reader(), leaf.ord(), leaf.doc_base(),
                                     TopParentMeta{static_cast<int32_t>(leaves_.size()), max_doc_, identity_});
        }
    }

    [[nodiscard]] Result<int32_t> max_doc() const override { return max_doc_; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return num_docs_; }

    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override { return nullptr; }
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override { return nullptr; }

    [[nodiscard]] Result<int32_t> doc_freq(const Term &term) override {
        return aggregate<int32_t>(term.field(), [&term](LeafReader &leaf) { return leaf.doc_freq(term); });
    }

    [[nodiscard]] Result<int64_t> total_term_freq(const Term &term) override {
        return aggregate<int64_t>(term.field(), [&term](LeafReader &leaf) { return leaf.total_term_freq(term); });
    }

    [[nodiscard]] Result<int64_t> sum_doc_freq(std::string_view field) override {
        return aggregate<int64_t>(field, [field](LeafReader &leaf) { return leaf.sum_doc_freq(field); });
    }

    [[nodiscard]] Result<int32_t> doc_count(std::string_view field) override {
        return aggregate<int32_t>(field, [field](LeafReader &leaf) { return leaf.doc_count(field); });
    }

    [[nodiscard]] Result<int64_t> sum_total_term_freq(std::string_view field) override {
        return aggregate<int64_t>(field, [field](LeafReader &leaf) { return leaf.sum_total_term_freq(field); });
    }

    [[nodiscard]] Result<std::span<const LeafReaderContext>> leaves() const override {
        return std::span<const LeafReaderContext>(leaves_);
    }

    [[nodiscard]] const Identity &identity() const override { return identity_; }

private:
    template <class T, class Fn>
    [[nodiscard]] Result<T> aggregate(std::string_view /*field*/, Fn &&per_leaf) {
        T sum{};
        for (auto &leaf : leaves_) {
            auto value = per_leaf(*leaf.reader());
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            sum = static_cast<T>(sum + value.value());
        }
        return sum;
    }

    std::vector<std::unique_ptr<LeafReader>> readers_;
    std::vector<LeafReaderContext> leaves_;
    Identity identity_{};
    int32_t max_doc_{0};
    int32_t num_docs_{0};
};

/// Two leaves: `body` holds "foo" in both, "bar" only in the first.
std::shared_ptr<IndexReader> make_reader() {
    std::vector<std::unique_ptr<LeafReader>> leaves;
    leaves.push_back(std::make_unique<FakeLeafReader>("body", std::map<std::string, TermStat>{
                                                                  {"bar", TermStat{1, 2}},
                                                                  {"foo", TermStat{2, 3}},
                                                          },
                                                      /*max_doc=*/4, /*num_docs=*/4, /*ord=*/0));
    leaves.push_back(std::make_unique<FakeLeafReader>("body", std::map<std::string, TermStat>{
                                                                  {"foo", TermStat{3, 5}},
                                                          },
                                                      /*max_doc=*/6, /*num_docs=*/5, /*ord=*/1));
    return std::make_shared<FakeCompositeReader>(std::move(leaves));
}

} // namespace

TEST(IndexSearcherStatisticsTest, CollectionStatisticsAggregateOverLeaves) {
    IndexSearcher searcher(make_reader());

    auto stats = searcher.collection_statistics("body");
    ASSERT_TRUE(stats.has_value());
    ASSERT_TRUE(stats.value().has_value());
    EXPECT_EQ(stats.value()->field(), "body");
    EXPECT_EQ(stats.value()->max_doc(), 10);
    EXPECT_EQ(stats.value()->doc_count(), 6);   // 3 + 3
    EXPECT_EQ(stats.value()->sum_doc_freq(), 6); // 3 + 3
    EXPECT_EQ(stats.value()->sum_total_term_freq(), 10); // 5 + 5
}

TEST(IndexSearcherStatisticsTest, MissingFieldHasNoStatistics) {
    IndexSearcher searcher(make_reader());

    auto stats = searcher.collection_statistics("title");
    ASSERT_TRUE(stats.has_value());
    EXPECT_FALSE(stats.value().has_value());
}

TEST(IndexSearcherStatisticsTest, TermStatisticsAreValidated) {
    IndexSearcher searcher(make_reader());

    auto stats = searcher.term_statistics(Term::from_text("body", "foo"), 5, 8);
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats.value().doc_freq(), 5);
    EXPECT_EQ(stats.value().total_term_freq(), 8);

    // doc_freq exceeds total_term_freq: rejected, like the reference implementation.
    auto invalid = searcher.term_statistics(Term::from_text("body", "foo"), 9, 8);
    EXPECT_FALSE(invalid.has_value());
}

TEST(IndexSearcherStatisticsTest, DocFreqAndTotalTermFreqDelegateToTheReader) {
    IndexSearcher searcher(make_reader());

    EXPECT_EQ(searcher.doc_freq(Term::from_text("body", "foo")).value(), 5);
    EXPECT_EQ(searcher.doc_freq(Term::from_text("body", "bar")).value(), 1);
    EXPECT_EQ(searcher.doc_freq(Term::from_text("body", "zoo")).value(), 0);
    EXPECT_EQ(searcher.total_term_freq(Term::from_text("body", "foo")).value(), 8);
    EXPECT_EQ(searcher.total_term_freq(Term::from_text("body", "bar")).value(), 2);
}

TEST(TermStatesTest, BuildWithStatsRegistersEveryLeaf) {
    IndexSearcher searcher(make_reader());

    auto states = build_term_states(searcher, Term::from_text("body", "foo"), /*needs_stats=*/true);
    ASSERT_TRUE(states.has_value());
    EXPECT_EQ(states->size(), 2U);
    EXPECT_EQ(states->doc_freq().value(), 5);
    EXPECT_EQ(states->total_term_freq().value(), 8);
    EXPECT_TRUE(states->was_built_for_id(searcher.top_reader_identity()));

    auto leaves = searcher.leaves().value();
    auto prepared = states->get(leaves[1]);
    ASSERT_TRUE(prepared.has_value());
    ASSERT_TRUE(prepared.value().has_value());
    auto state = states->resolve(*prepared.value());
    ASSERT_TRUE(state.has_value());
    ASSERT_TRUE(state.value().has_value());
    // Leaf 1 has a single term, so "foo" sits at ordinal 0: 1 * 1000 + 0.
    EXPECT_EQ((*state.value())->ord().value(), 1000);
}

TEST(TermStatesTest, MissingTermResolvesToNothingEverywhere) {
    IndexSearcher searcher(make_reader());

    auto states = build_term_states(searcher, Term::from_text("body", "zoo"), /*needs_stats=*/true);
    ASSERT_TRUE(states.has_value());
    EXPECT_EQ(states->doc_freq().value(), 0);
    EXPECT_EQ(states->total_term_freq().value(), 0);

    auto leaves = searcher.leaves().value();
    auto prepared = states->get(leaves[0]);
    ASSERT_TRUE(prepared.has_value());
    EXPECT_FALSE(prepared.value().has_value());
}

TEST(TermStatesTest, BuildWithoutStatsResolvesLazilyPerLeaf) {
    IndexSearcher searcher(make_reader());

    auto states = build_term_states(searcher, Term::from_text("body", "bar"), /*needs_stats=*/false);
    ASSERT_TRUE(states.has_value());
    // Without statistics the frequencies are not accumulated, and asking for
    // them is an error (the reference implementation's contract).
    EXPECT_FALSE(states->doc_freq().has_value());
    EXPECT_FALSE(states->total_term_freq().has_value());

    auto leaves = searcher.leaves().value();

    // Leaf 0 has the term: the first call prepares, resolve completes it.
    auto prepared = states->get(leaves[0]);
    ASSERT_TRUE(prepared.has_value());
    ASSERT_TRUE(prepared.value().has_value());
    EXPECT_FALSE(prepared.value()->is_ready());
    EXPECT_EQ(prepared.value()->ord(), 0);
    auto state = states->resolve(*prepared.value());
    ASSERT_TRUE(state.has_value());
    ASSERT_TRUE(state.value().has_value());
    EXPECT_EQ((*state.value())->ord().value(), 0);

    // A second lookup takes the ready path and yields the same state.
    auto again = states->get(leaves[0]);
    ASSERT_TRUE(again.has_value());
    ASSERT_TRUE(again.value().has_value());
    EXPECT_TRUE(again.value()->is_ready());
    auto again_state = states->resolve(*again.value());
    ASSERT_TRUE(again_state.has_value());
    ASSERT_TRUE(again_state.value().has_value());
    EXPECT_EQ((*again_state.value())->ord().value(), 0);

    // Leaf 1 has no "bar": the seek is prepared first, and only `resolve`
    // discovers the absence.
    auto missing = states->get(leaves[1]);
    ASSERT_TRUE(missing.has_value());
    ASSERT_TRUE(missing.value().has_value());
    auto missing_state = states->resolve(*missing.value());
    ASSERT_TRUE(missing_state.has_value());
    EXPECT_FALSE(missing_state.value().has_value());

    // The finished (and empty) lookup is remembered: no second seek.
    auto missing_again = states->get(leaves[1]);
    ASSERT_TRUE(missing_again.has_value());
    EXPECT_FALSE(missing_again.value().has_value());
}

TEST(TermStatesTest, ClearDropsStatesAndStatistics) {
    IndexSearcher searcher(make_reader());

    auto states = build_term_states(searcher, Term::from_text("body", "foo"), /*needs_stats=*/true);
    ASSERT_TRUE(states.has_value());
    EXPECT_EQ(states->doc_freq().value(), 5);

    states->clear();
    EXPECT_EQ(states->doc_freq().value(), 0);
    EXPECT_EQ(states->total_term_freq().value(), 0);

    auto leaves = searcher.leaves().value();
    auto prepared = states->get(leaves[0]);
    ASSERT_TRUE(prepared.has_value());
    EXPECT_FALSE(prepared.value().has_value());
}

TEST(TermStatesTest, WithStateAndStatsRegistersOneLeaf) {
    auto reader = make_reader();
    auto states = TermStates::with_state_and_stats(*reader, TermStateEnum(OrdTermState(7)), /*ord=*/1,
                                                   /*doc_freq=*/3, /*total_term_freq=*/4);
    ASSERT_TRUE(states.has_value());
    EXPECT_EQ(states->doc_freq().value(), 3);
    EXPECT_EQ(states->total_term_freq().value(), 4);

    auto leaves = reader->leaves().value();
    auto prepared = states->get(leaves[1]);
    ASSERT_TRUE(prepared.has_value());
    ASSERT_TRUE(prepared.value().has_value());
    EXPECT_TRUE(prepared.value()->is_ready());
    auto state = states->resolve(*prepared.value());
    ASSERT_TRUE(state.has_value());
    ASSERT_TRUE(state.value().has_value());
    EXPECT_EQ((*state.value())->ord().value(), 7);

    // The other leaf has no state and no term to resolve.
    auto other = states->get(leaves[0]);
    ASSERT_TRUE(other.has_value());
    EXPECT_FALSE(other.value().has_value());
}

TEST(TermStatesTest, WasBuiltForRejectsAnotherReader) {
    IndexSearcher searcher(make_reader());
    IndexSearcher other(make_reader());

    auto states = build_term_states(searcher, Term::from_text("body", "foo"), /*needs_stats=*/false);
    ASSERT_TRUE(states.has_value());

    auto own_leaves = searcher.leaves().value();
    auto other_leaves = other.leaves().value();
    EXPECT_TRUE(states->was_built_for(own_leaves[0].top_parent()));
    EXPECT_FALSE(states->was_built_for(other_leaves[0].top_parent()));
}

TEST(TermStatesTest, OutOfRangeLeafOrdIsRejected) {
    auto reader = make_reader();
    auto states = TermStates::create(*reader);
    ASSERT_TRUE(states.has_value());

    auto leaves = reader->leaves().value();
    const LeafReaderContext bogus(leaves[0].reader(), /*ord=*/9, /*doc_base=*/0, leaves[0].top_parent());
    EXPECT_FALSE(states->get(bogus).has_value());
}
