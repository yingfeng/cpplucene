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
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.multi_term_queries;

import lucene_core;

using namespace lucene;

namespace {

/// [`TermsEnum`] over a sorted list of term texts; forward only, plus the seeks
/// [`SingleTermsEnum`] needs.
class VectorTermsEnum final : public TermsEnum {
public:
    explicit VectorTermsEnum(std::vector<std::string> terms) : terms_(std::move(terms)) {}

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

    [[nodiscard]] Result<int32_t> doc_freq() override { return 1; }
    [[nodiscard]] Result<int64_t> total_term_freq() override { return 1; }

    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>> postings_with_flags(std::unique_ptr<PostingsEnum> /*reuse*/,
                                                                           int32_t /*flags*/) override {
        return err(LuceneError::unsupported_operation("VectorTermsEnum has no postings"));
    }

    [[nodiscard]] Result<std::unique_ptr<ImpactsEnum>> impacts(int32_t /*flags*/) override {
        return err(LuceneError::unsupported_operation("VectorTermsEnum has no impacts"));
    }

    [[nodiscard]] Result<AttributeSource *> attributes() override {
        return err(LuceneError::unsupported_operation("VectorTermsEnum has no attributes"));
    }
    [[nodiscard]] Result<AttributeSource *> attributes_mut() override {
        return err(LuceneError::unsupported_operation("VectorTermsEnum has no attributes"));
    }

    [[nodiscard]] Result<SeekStatus> seek_ceil(std::span<const uint8_t> target) override {
        const std::string text(reinterpret_cast<const char *>(target.data()), target.size());
        for (size_t i = 0; i < terms_.size(); ++i) {
            if (terms_[i] < text) {
                continue;
            }
            pos_ = i;
            return terms_[i] == text ? SeekStatus::Found : SeekStatus::NotFound;
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
    [[nodiscard]] const BytesRef &term_at(size_t index) const {
        cache_ = BytesRef::from_string(terms_[index]);
        return cache_;
    }

    std::vector<std::string> terms_;
    std::optional<size_t> pos_{};
    mutable BytesRef cache_{};
};

/// [`Terms`] over a sorted list of term texts.
class VectorTerms final : public Terms {
public:
    explicit VectorTerms(std::vector<std::string> terms) : terms_(std::move(terms)) {}

    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> iterator() const override {
        return std::unique_ptr<TermsEnum>(std::make_unique<VectorTermsEnum>(terms_));
    }

    [[nodiscard]] Result<int64_t> size() const override { return static_cast<int64_t>(terms_.size()); }
    [[nodiscard]] Result<int64_t> sum_total_term_freq() const override {
        return static_cast<int64_t>(terms_.size());
    }
    [[nodiscard]] Result<int64_t> sum_doc_freq() const override { return static_cast<int64_t>(terms_.size()); }
    [[nodiscard]] Result<int32_t> doc_count() const override { return static_cast<int32_t>(terms_.size()); }
    [[nodiscard]] bool has_freqs() const override { return false; }
    [[nodiscard]] bool has_offsets() const override { return false; }
    [[nodiscard]] bool has_positions() const override { return false; }
    [[nodiscard]] bool has_payloads() const override { return false; }

private:
    std::vector<std::string> terms_;
};

/// A [`LeafReader`] with one terms field and nothing else.
class TermsOnlyLeafReader final : public LeafReader {
public:
    explicit TermsOnlyLeafReader(std::vector<std::string> terms) : terms_(std::move(terms)) {}

    [[nodiscard]] Result<int32_t> max_doc() const override { return 1; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return 1; }

    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override { return nullptr; }
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override { return nullptr; }

    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view field) override {
        if (field != kField) {
            return std::unique_ptr<Terms>{};
        }
        return std::unique_ptr<Terms>(std::make_unique<VectorTerms>(terms_));
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
    std::vector<std::string> terms_;
    FieldInfos field_infos_{FieldInfos::none()};
};

/// The terms of the fake field, in byte order.
const std::vector<std::string> &dictionary() {
    static const std::vector<std::string> terms{"*", "FOO", "bar", "foo", "foobar", "food", "fool"};
    return terms;
}

IndexSearcher make_searcher() {
    std::shared_ptr<IndexReader> reader = std::make_shared<TermsOnlyLeafReader>(dictionary());
    return IndexSearcher(std::move(reader));
}

[[nodiscard]] Term make_term(std::string text) { return Term::from_text("body", text); }

/// The term texts the rewrite of `query` expands to; empty when the rewrite
/// matches nothing.
std::vector<std::string> expanded_terms(const Query &query) {
    const Query *inner = &query;
    if (const auto *constant_score = dynamic_cast<const ConstantScoreQuery *>(inner); constant_score != nullptr) {
        inner = constant_score->query().get();
    }
    std::vector<std::string> texts;
    const auto *boolean = dynamic_cast<const BooleanQuery *>(inner);
    if (boolean == nullptr) {
        return texts;
    }
    for (const BooleanClause &clause : boolean->clauses()) {
        const auto *term_query = dynamic_cast<const TermQuery *>(clause.query.get());
        if (term_query == nullptr) {
            ADD_FAILURE() << "a multi term rewrite has to produce term queries";
            continue;
        }
        texts.push_back(term_query->term().text());
    }
    return texts;
}

/// Rewrites `query` with a searcher over [`dictionary`] and returns the terms.
std::vector<std::string> rewrite_terms(const MultiTermQuery &query) {
    const IndexSearcher searcher = make_searcher();
    auto rewritten = query.rewrite(searcher);
    EXPECT_TRUE(rewritten.has_value()) << rewritten.error().to_string();
    if (!rewritten.has_value() || rewritten.value() == nullptr) {
        return {};
    }
    return expanded_terms(*rewritten.value());
}

// ---------------------------------------------------------------------------
// PrefixQuery
// ---------------------------------------------------------------------------

TEST(MultiTermQueriesTest, PrefixExpandsToTheTermsStartingWithIt) {
    const PrefixQuery query("body", "foo");
    const IndexSearcher searcher = make_searcher();
    auto rewritten = query.rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value());
    ASSERT_NE(rewritten.value(), nullptr);
    // The default rewrite method is the constant score one.
    EXPECT_NE(dynamic_cast<const ConstantScoreQuery *>(rewritten.value().get()), nullptr);
    EXPECT_EQ(expanded_terms(*rewritten.value()),
              (std::vector<std::string>{"foo", "foobar", "food", "fool"}));
}

TEST(MultiTermQueriesTest, PrefixOfNothingMatchesEveryTerm) {
    const PrefixQuery query("body", "");
    EXPECT_EQ(rewrite_terms(query), dictionary());
}

TEST(MultiTermQueriesTest, PrefixWithNoMatchRewritesToMatchNoDocs) {
    const PrefixQuery query("body", "zzz");
    const IndexSearcher searcher = make_searcher();
    auto rewritten = query.rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value());
    ASSERT_NE(rewritten.value(), nullptr);
    EXPECT_NE(dynamic_cast<const MatchNoDocsQuery *>(rewritten.value().get()), nullptr);
}

TEST(MultiTermQueriesTest, PrefixIsCaseSensitive) {
    const PrefixQuery query("body", "FO");
    EXPECT_EQ(rewrite_terms(query), (std::vector<std::string>{"FOO"}));
}

TEST(MultiTermQueriesTest, PrefixOnAnUnknownFieldMatchesNothing) {
    const PrefixQuery query("title", "foo");
    EXPECT_TRUE(rewrite_terms(query).empty());
}

// ---------------------------------------------------------------------------
// WildcardQuery
// ---------------------------------------------------------------------------

TEST(MultiTermQueriesTest, WildcardStarMatchesAnySequence) {
    auto query = WildcardQuery::create(make_term("foo*"));
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(rewrite_terms(query.value()), (std::vector<std::string>{"foo", "foobar", "food", "fool"}));
}

TEST(MultiTermQueriesTest, WildcardQuestionMatchesOneCharacter) {
    auto query = WildcardQuery::create(make_term("fo?l"));
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(rewrite_terms(query.value()), (std::vector<std::string>{"fool"}));
}

TEST(MultiTermQueriesTest, WildcardInTheMiddleMatchesTheDictionary) {
    auto query = WildcardQuery::create(make_term("*o*"));
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(rewrite_terms(query.value()), (std::vector<std::string>{"foo", "foobar", "food", "fool"}));
}

TEST(MultiTermQueriesTest, WildcardEscapesReservedCharacters) {
    auto query = WildcardQuery::create(make_term("\\*"));
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(rewrite_terms(query.value()), (std::vector<std::string>{"*"}));
}

TEST(MultiTermQueriesTest, WildcardWithoutAnyWildcardMatchesTheSingleTerm) {
    auto query = WildcardQuery::create(make_term("bar"));
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(rewrite_terms(query.value()), (std::vector<std::string>{"bar"}));
}

TEST(MultiTermQueriesTest, WildcardWithoutAnyWildcardStillHasNormalForm) {
    // `"bar"` compiles into the single-term special case, which the rewrite has
    // to expand just like the general automaton.
    auto query = WildcardQuery::create(make_term("nope"));
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_TRUE(rewrite_terms(query.value()).empty());
}

TEST(MultiTermQueriesTest, WildcardScoringRewriteDoesNotWrapTheBooleanQuery) {
    auto query = WildcardQuery::with_rewrite(make_term("foo*"),
                                             static_cast<int32_t>(Operations::DEFAULT_DETERMINIZE_WORK_LIMIT),
                                             MultiTermRewriteMethod::kScoringBoolean);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    const IndexSearcher searcher = make_searcher();
    auto rewritten = query.value().rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value());
    ASSERT_NE(rewritten.value(), nullptr);
    EXPECT_EQ(dynamic_cast<const ConstantScoreQuery *>(rewritten.value().get()), nullptr);
    EXPECT_NE(dynamic_cast<const BooleanQuery *>(rewritten.value().get()), nullptr);
}

TEST(MultiTermQueriesTest, WildcardToStringPrintsThePattern) {
    auto query = WildcardQuery::create(make_term("foo*"));
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(query.value().to_string(""), "body:foo*");
    EXPECT_EQ(query.value().to_string("body"), "foo*");
    EXPECT_EQ(query.value().to_string("title"), "body:foo*");
}

TEST(MultiTermQueriesTest, WildcardEqualityComparesFieldPatternAndRewrite) {
    auto left = WildcardQuery::create(make_term("foo*"));
    auto same = WildcardQuery::create(make_term("foo*"));
    auto other = WildcardQuery::create(make_term("foo?"));
    auto scoring = WildcardQuery::with_rewrite(make_term("foo*"),
                                               static_cast<int32_t>(Operations::DEFAULT_DETERMINIZE_WORK_LIMIT),
                                               MultiTermRewriteMethod::kScoringBoolean);
    ASSERT_TRUE(left.has_value());
    ASSERT_TRUE(same.has_value());
    ASSERT_TRUE(other.has_value());
    ASSERT_TRUE(scoring.has_value());
    EXPECT_TRUE(left.value().equals(same.value()));
    EXPECT_EQ(left.value().hash(), same.value().hash());
    EXPECT_FALSE(left.value().equals(other.value()));
    EXPECT_FALSE(left.value().equals(scoring.value()));
}

TEST(MultiTermQueriesTest, WildcardAutomatonAcceptsExactlyThePattern) {
    auto automaton = WildcardQuery::to_automaton(make_term("fo?d"), 10'000);
    ASSERT_TRUE(automaton.has_value()) << automaton.error().to_string();
    // The pattern is deterministic already (no `*`), so the automaton compiled
    // from it must accept only the four character terms `f`, `o`, `?`, `d`.
    const std::vector<std::string> accepted{"food", "fold", "ford", "fozd"};
    const std::vector<std::string> rejected{"fo", "foods", "foo", "foodie", "fund"};
    auto compiled = CompiledAutomaton::with_binary(automaton.value(), false, true, false);
    ASSERT_TRUE(compiled.has_value()) << compiled.error().to_string();
    auto runnable = compiled.value().get_automaton();
    ASSERT_TRUE(runnable.has_value());
    for (const std::string &text : accepted) {
        auto matched = runnable.value().run(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()),
                                                                     text.size()),
                                            0, text.size());
        ASSERT_TRUE(matched.has_value());
        EXPECT_TRUE(matched.value()) << text;
    }
    for (const std::string &text : rejected) {
        auto matched = runnable.value().run(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()),
                                                                     text.size()),
                                            0, text.size());
        ASSERT_TRUE(matched.has_value());
        EXPECT_FALSE(matched.value()) << text;
    }
}

// ---------------------------------------------------------------------------
// RegexpQuery
// ---------------------------------------------------------------------------

TEST(MultiTermQueriesTest, RegexpDotMatchesOneCharacter) {
    auto query = RegexpQuery::create(make_term("foo."));
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(rewrite_terms(query.value()), (std::vector<std::string>{"food", "fool"}));
}

TEST(MultiTermQueriesTest, RegexpAlternationMatchesEitherBranch) {
    auto query = RegexpQuery::create(make_term("bar|fool"));
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(rewrite_terms(query.value()), (std::vector<std::string>{"bar", "fool"}));
}

TEST(MultiTermQueriesTest, RegexpIsCaseSensitiveByDefault) {
    auto query = RegexpQuery::create(make_term("foo"));
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(rewrite_terms(query.value()), (std::vector<std::string>{"foo"}));
}

TEST(MultiTermQueriesTest, RegexpCaseInsensitiveFlagMatchesBothCases) {
    auto query =
            RegexpQuery::with_syntax_and_match_flags(make_term("FOO"), RegExp::ALL, RegExp::ASCII_CASE_INSENSITIVE,
                                                      static_cast<int32_t>(Operations::DEFAULT_DETERMINIZE_WORK_LIMIT));
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(rewrite_terms(query.value()), (std::vector<std::string>{"FOO", "foo"}));
}

TEST(MultiTermQueriesTest, RegexpRecordsItsFlags) {
    auto query = RegexpQuery::with_flags(make_term("foo"), RegExp::NONE);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(query.value().syntax_flags(), RegExp::NONE);
    EXPECT_EQ(query.value().match_flags(), 0);
    EXPECT_TRUE(query.value().do_determinization());
    EXPECT_EQ(query.value().determinize_work_limit(),
              static_cast<int32_t>(Operations::DEFAULT_DETERMINIZE_WORK_LIMIT));
}

TEST(MultiTermQueriesTest, RegexpToStringPrintsThePatternBetweenSlashes) {
    auto query = RegexpQuery::create(make_term("ba."));
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(query.value().to_string(""), "body:/ba./");
    EXPECT_EQ(query.value().to_string("body"), "/ba./");
    EXPECT_EQ(query.value().to_string("title"), "body:/ba./");
}

TEST(MultiTermQueriesTest, RegexpEqualityComparesTheFlags) {
    auto all = RegexpQuery::create(make_term("ba."));
    auto same = RegexpQuery::create(make_term("ba."));
    auto restricted = RegexpQuery::with_flags(make_term("ba."), RegExp::NONE);
    ASSERT_TRUE(all.has_value());
    ASSERT_TRUE(same.has_value());
    ASSERT_TRUE(restricted.has_value());
    EXPECT_TRUE(all.value().equals(same.value()));
    EXPECT_EQ(all.value().hash(), same.value().hash());
    EXPECT_FALSE(all.value().equals(restricted.value()));
}

TEST(MultiTermQueriesTest, RegexpParseErrorIsReported) {
    auto query = RegexpQuery::create(make_term("foo("));
    EXPECT_FALSE(query.has_value());
}

TEST(MultiTermQueriesTest, RegexpWithoutWildcardMatchesTheSingleTerm) {
    auto query = RegexpQuery::create(make_term("foobar"));
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(rewrite_terms(query.value()), (std::vector<std::string>{"foobar"}));
}

// ---------------------------------------------------------------------------
// AutomatonQuery
// ---------------------------------------------------------------------------

TEST(MultiTermQueriesTest, AutomatonQueryExpandsTheTermsTheAutomatonAccepts) {
    auto automaton = Automata::make_any_string();
    ASSERT_TRUE(automaton.has_value()) << automaton.error().to_string();
    auto query = AutomatonQuery::from_automaton(make_term("<all>"), std::move(automaton.value()));
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(rewrite_terms(query.value()), dictionary());
    EXPECT_EQ(query.value().to_string("body"), "AutomatonQuery(<all>)");
}

TEST(MultiTermQueriesTest, AutomatonQueryOfNothingMatchesNothing) {
    // The complement of "any string" accepts no string at all.
    auto any = Automata::make_any_string();
    ASSERT_TRUE(any.has_value());
    auto automaton = Operations::complement(any.value(), Operations::DEFAULT_DETERMINIZE_WORK_LIMIT);
    ASSERT_TRUE(automaton.has_value()) << automaton.error().to_string();
    auto query = AutomatonQuery::from_automaton(make_term("<none>"), std::move(automaton.value()));
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_TRUE(rewrite_terms(query.value()).empty());
}

TEST(MultiTermQueriesTest, AutomatonQueryIsNotEqualToAConcreteQuery) {
    auto automaton = Automata::make_any_string();
    ASSERT_TRUE(automaton.has_value());
    auto automaton_query = AutomatonQuery::from_automaton(make_term("<all>"), std::move(automaton.value()));
    auto wildcard = WildcardQuery::create(make_term("*"));
    ASSERT_TRUE(automaton_query.has_value());
    ASSERT_TRUE(wildcard.has_value());
    EXPECT_FALSE(automaton_query.value().equals(wildcard.value()));
}

// ---------------------------------------------------------------------------
// TermRangeQuery
// ---------------------------------------------------------------------------

TEST(MultiTermQueriesTest, TermRangeIncludesBothEndpoints) {
    auto query = TermRangeQuery::create_string_range("body", "foo", "fool", true, true);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(rewrite_terms(query.value()), (std::vector<std::string>{"foo", "foobar", "food", "fool"}));
}

TEST(MultiTermQueriesTest, TermRangeExcludesAnExclusiveEndpoint) {
    auto lower_excluded = TermRangeQuery::create_string_range("body", "foo", "fool", false, true);
    ASSERT_TRUE(lower_excluded.has_value()) << lower_excluded.error().to_string();
    EXPECT_EQ(rewrite_terms(lower_excluded.value()), (std::vector<std::string>{"foobar", "food", "fool"}));

    auto upper_excluded = TermRangeQuery::create_string_range("body", "foo", "fool", true, false);
    ASSERT_TRUE(upper_excluded.has_value()) << upper_excluded.error().to_string();
    EXPECT_EQ(rewrite_terms(upper_excluded.value()), (std::vector<std::string>{"foo", "foobar", "food"}));

    auto both_excluded = TermRangeQuery::create_string_range("body", "foo", "fool", false, false);
    ASSERT_TRUE(both_excluded.has_value()) << both_excluded.error().to_string();
    EXPECT_EQ(rewrite_terms(both_excluded.value()), (std::vector<std::string>{"foobar", "food"}));
}

TEST(MultiTermQueriesTest, TermRangeWithAnOpenLowerEndpoint) {
    auto query = TermRangeQuery::create_string_range("body", std::nullopt, "bar", false, true);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(rewrite_terms(query.value()), (std::vector<std::string>{"*", "FOO", "bar"}));
}

TEST(MultiTermQueriesTest, TermRangeWithAnOpenUpperEndpoint) {
    auto query = TermRangeQuery::create_string_range("body", "foobar", std::nullopt, true, false);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(rewrite_terms(query.value()), (std::vector<std::string>{"foobar", "food", "fool"}));
}

TEST(MultiTermQueriesTest, TermRangeOverEverythingMatchesEverything) {
    auto query = TermRangeQuery::create_string_range("body", std::nullopt, std::nullopt, false, false);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(rewrite_terms(query.value()), dictionary());
}

TEST(MultiTermQueriesTest, TermRangeWithNoTermInItMatchesNothing) {
    auto query = TermRangeQuery::create_string_range("body", "aaa", "aab", true, true);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_TRUE(rewrite_terms(query.value()).empty());
}

TEST(MultiTermQueriesTest, TermRangeToStringPrintsTheEndpoints) {
    auto inclusive = TermRangeQuery::create_string_range("body", "foo", "fool", true, true);
    ASSERT_TRUE(inclusive.has_value());
    EXPECT_EQ(inclusive.value().to_string(""), "body:[foo TO fool]");
    EXPECT_EQ(inclusive.value().to_string("body"), "[foo TO fool]");

    auto exclusive = TermRangeQuery::create_string_range("body", "foo", "fool", false, false);
    ASSERT_TRUE(exclusive.has_value());
    EXPECT_EQ(exclusive.value().to_string("body"), "{foo TO fool}");

    auto open = TermRangeQuery::create_string_range("body", std::nullopt, "fool", true, true);
    ASSERT_TRUE(open.has_value());
    EXPECT_EQ(open.value().to_string("body"), "[* TO fool]");
}

TEST(MultiTermQueriesTest, TermRangeToStringEscapesALiteralStar) {
    auto query = TermRangeQuery::create_string_range("body", "*", "fool", true, true);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(query.value().to_string("body"), "[\\* TO fool]");
}

TEST(MultiTermQueriesTest, TermRangeEqualityComparesBoundsAndInclusivity) {
    auto inclusive = TermRangeQuery::create_string_range("body", "foo", "fool", true, true);
    auto same = TermRangeQuery::create_string_range("body", "foo", "fool", true, true);
    auto exclusive = TermRangeQuery::create_string_range("body", "foo", "fool", true, false);
    auto other_bound = TermRangeQuery::create_string_range("body", "foo", "food", true, true);
    ASSERT_TRUE(inclusive.has_value());
    ASSERT_TRUE(same.has_value());
    ASSERT_TRUE(exclusive.has_value());
    ASSERT_TRUE(other_bound.has_value());
    EXPECT_TRUE(inclusive.value().equals(same.value()));
    EXPECT_EQ(inclusive.value().hash(), same.value().hash());
    EXPECT_FALSE(inclusive.value().equals(exclusive.value()));
    EXPECT_FALSE(inclusive.value().equals(other_bound.value()));
}

TEST(MultiTermQueriesTest, TermRangeAcceptsBinaryBounds) {
    // The range automaton works on bytes, not code points, so a term with bytes
    // beyond ASCII has to be found as well.
    const std::string lower = "\xC3\xA0"; // "à"
    auto query = TermRangeQuery::create("body", BytesRef::from_string(lower), std::nullopt, true, true);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    // Nothing in the dictionary sorts at or after "à", which starts with 0xC3.
    EXPECT_TRUE(rewrite_terms(query.value()).empty());
}

// ---------------------------------------------------------------------------
// FuzzyQuery
// ---------------------------------------------------------------------------

TEST(MultiTermQueriesTest, FuzzyMatchesWithinTheEditDistance) {
    auto query = FuzzyQuery::create(make_term("food"));
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    // "food" (0), "fool" (1) and "foo" (1) are within two edits; "foobar" needs
    // three and "FOO" needs three too.
    EXPECT_EQ(rewrite_terms(query.value()), (std::vector<std::string>{"foo", "food", "fool"}));
}

TEST(MultiTermQueriesTest, FuzzyWithOneEditNarrowsTheMatches) {
    auto query = FuzzyQuery::with_max_edits(make_term("fool"), 1);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(rewrite_terms(query.value()), (std::vector<std::string>{"foo", "food", "fool"}));
}

TEST(MultiTermQueriesTest, FuzzyWithAFixedPrefixKeepsItExact) {
    auto query = FuzzyQuery::with_max_edits_and_prefix(make_term("food"), 1, 3);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(rewrite_terms(query.value()), (std::vector<std::string>{"foo", "food", "fool"}));

    // A prefix of the whole term and one edit leaves only that term.
    auto narrow = FuzzyQuery::with_max_edits_and_prefix(make_term("bar"), 1, 3);
    ASSERT_TRUE(narrow.has_value()) << narrow.error().to_string();
    EXPECT_EQ(rewrite_terms(narrow.value()), (std::vector<std::string>{"bar"}));
}

TEST(MultiTermQueriesTest, FuzzyWithNoEditsIsTheTermItself) {
    auto query = FuzzyQuery::with_max_edits(make_term("foobar"), 0);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(rewrite_terms(query.value()), (std::vector<std::string>{"foobar"}));

    auto missing = FuzzyQuery::with_max_edits(make_term("absent"), 0);
    ASSERT_TRUE(missing.has_value()) << missing.error().to_string();
    EXPECT_TRUE(rewrite_terms(missing.value()).empty());
}

TEST(MultiTermQueriesTest, FuzzyRejectsAnUnsupportedEditDistance) {
    auto too_many =
            FuzzyQuery::with_max_edits(make_term("food"), LevenshteinAutomata::MAXIMUM_SUPPORTED_DISTANCE + 1);
    ASSERT_FALSE(too_many.has_value());
    EXPECT_EQ(too_many.error().kind(), ErrorKind::kIllegalArgument);

    auto negative = FuzzyQuery::with_max_edits(make_term("food"), -1);
    ASSERT_FALSE(negative.has_value());
    EXPECT_EQ(negative.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(MultiTermQueriesTest, FuzzyRejectsZeroMaxExpansions) {
    auto query = FuzzyQuery::with_options(make_term("food"), 2, 0, 0, true);
    ASSERT_FALSE(query.has_value());
    EXPECT_EQ(query.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(MultiTermQueriesTest, FuzzyToStringPrintsTheEditDistance) {
    auto query = FuzzyQuery::with_max_edits(make_term("food"), 1);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(query.value().to_string(""), "body:food~1");
    EXPECT_EQ(query.value().to_string("body"), "food~1");
    EXPECT_EQ(query.value().to_string("title"), "body:food~1");
}

TEST(MultiTermQueriesTest, FuzzyEqualityComparesTheOptions) {
    auto left = FuzzyQuery::with_max_edits(make_term("food"), 1);
    auto same = FuzzyQuery::with_max_edits(make_term("food"), 1);
    auto more_edits = FuzzyQuery::with_max_edits(make_term("food"), 2);
    auto prefix = FuzzyQuery::with_max_edits_and_prefix(make_term("food"), 1, 2);
    ASSERT_TRUE(left.has_value());
    ASSERT_TRUE(same.has_value());
    ASSERT_TRUE(more_edits.has_value());
    ASSERT_TRUE(prefix.has_value());
    EXPECT_TRUE(left.value().equals(same.value()));
    EXPECT_EQ(left.value().hash(), same.value().hash());
    EXPECT_FALSE(left.value().equals(more_edits.value()));
    EXPECT_FALSE(left.value().equals(prefix.value()));
}

TEST(MultiTermQueriesTest, FuzzyFloatToEditsScalesWithTheTermLength) {
    EXPECT_EQ(FuzzyQuery::float_to_edits(1.0F, 5), 1);
    EXPECT_EQ(FuzzyQuery::float_to_edits(0.0F, 5), 0);
    EXPECT_EQ(FuzzyQuery::float_to_edits(0.79F, 5), 1);
    // `0.8f` is slightly above 0.8, so `(1 - 0.8) * 5` truncates to 0, exactly
    // like the reference implementation which does the arithmetic in f64.
    EXPECT_EQ(FuzzyQuery::float_to_edits(0.8F, 5), 0);
    EXPECT_EQ(FuzzyQuery::float_to_edits(0.5F, 5), 2);
    // Clamped to the supported distance.
    EXPECT_EQ(FuzzyQuery::float_to_edits(0.1F, 5), LevenshteinAutomata::MAXIMUM_SUPPORTED_DISTANCE);
    EXPECT_EQ(FuzzyQuery::float_to_edits(9.0F, 5), LevenshteinAutomata::MAXIMUM_SUPPORTED_DISTANCE);
}

TEST(MultiTermQueriesTest, FuzzyAutomatonBuilderBuildsOneAutomatonPerDistance) {
    auto builder = FuzzyAutomatonBuilder::create("food", 2, 2, true);
    ASSERT_TRUE(builder.has_value()) << builder.error().to_string();
    EXPECT_EQ(builder.value().term_length(), 4);
    EXPECT_EQ(builder.value().max_edits(), 2);

    auto automata = builder.value().build_automaton_set();
    ASSERT_TRUE(automata.has_value()) << automata.error().to_string();
    EXPECT_EQ(automata.value().size(), 3);

    auto max_edit = builder.value().build_max_edit_automaton();
    ASSERT_TRUE(max_edit.has_value()) << max_edit.error().to_string();
}

TEST(MultiTermQueriesTest, FuzzyAutomatonBuilderRejectsAnUnsupportedDistance) {
    auto builder = FuzzyAutomatonBuilder::create("food", 3, 0, true);
    ASSERT_FALSE(builder.has_value());
    EXPECT_EQ(builder.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(MultiTermQueriesTest, FuzzyAutomatonBuilderClipsThePrefixToTheTerm) {
    // A prefix longer than the term means the whole term must match exactly.
    auto builder = FuzzyAutomatonBuilder::create("foo", 2, 99, true);
    ASSERT_TRUE(builder.has_value()) << builder.error().to_string();
    EXPECT_EQ(builder.value().term_length(), 3);

    auto automaton = builder.value().build_max_edit_automaton();
    ASSERT_TRUE(automaton.has_value()) << automaton.error().to_string();
    auto runnable = automaton.value().get_automaton();
    ASSERT_TRUE(runnable.has_value());
    const std::string text = "foo";
    auto matched = runnable.value().run(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()),
                                                                text.size()),
                                       0, text.size());
    ASSERT_TRUE(matched.has_value());
    EXPECT_TRUE(matched.value());
}

} // namespace
