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
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.term_automaton_query;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kId = "id";
constexpr std::string_view kText = "text";

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqsAndPositions).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

FieldType keyword_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::Docs).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

/// `id` -> the tokens of the document, in order.  The corpus is chosen so that
/// position handling is observable: "b a" holds both query terms in the wrong
/// order, and "a b b c" holds one of them twice.
const std::vector<std::pair<std::string, std::string>> &corpus() {
    static const std::vector<std::pair<std::string, std::string>> documents{
            {"1", "a b c"},
            {"2", "b a"},
            {"3", "a c"},
            {"4", "a b b c"},
            {"5", "x y"},
    };
    return documents;
}

void build_index(Directory &directory) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    for (const auto &[id, text] : corpus()) {
        Document doc;
        doc.add(std::make_unique<Field>(std::string(kId), FieldValue{id}, keyword_type()));
        doc.add(std::make_unique<Field>(std::string(kText), FieldValue{text}, text_type()));
        ASSERT_TRUE(writer.value()->add_document(std::move(doc)).has_value());
    }
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());
}

/// A query whose initial state `0` already exists.
TermAutomatonQuery make_query(std::string field) {
    TermAutomatonQuery query{std::move(field)};
    EXPECT_EQ(query.create_state(), 0);
    return query;
}

/// Creates `count` states after the initial one.
void add_states(TermAutomatonQuery &query, int32_t count) {
    for (int32_t i = 0; i < count; ++i) {
        (void)query.create_state();
    }
}

/// A term automaton that matches `terms` in exactly this order.
TermAutomatonQuery in_order(std::string field, std::span<const std::string> terms) {
    TermAutomatonQuery query = make_query(std::move(field));
    for (size_t i = 0; i < terms.size(); ++i) {
        // States have to exist before a transition can point at them.
        add_states(query, 1);
        EXPECT_TRUE(query.add_transition(static_cast<int32_t>(i), static_cast<int32_t>(i + 1), terms[i]).has_value());
    }
    query.set_accept(static_cast<int32_t>(terms.size()), true);
    auto finished = query.finish();
    EXPECT_TRUE(finished.has_value()) << (finished ? "" : finished.error().to_string());
    return query;
}

/// "a", any term, "c".
TermAutomatonQuery a_any_c() {
    TermAutomatonQuery query = make_query(std::string(kText));
    add_states(query, 3);
    EXPECT_TRUE(query.add_transition(0, 1, "a").has_value());
    EXPECT_TRUE(query.add_any_transition(1, 2).has_value());
    EXPECT_TRUE(query.add_transition(2, 3, "c").has_value());
    query.set_accept(3, true);
    auto finished = query.finish();
    EXPECT_TRUE(finished.has_value()) << (finished ? "" : finished.error().to_string());
    return query;
}

/// "a", optionally followed by "b".  State `1` accepts and has an outgoing
/// transition, so the query is never rewritten into a phrase and the automaton
/// weight is the one that answers.
TermAutomatonQuery a_or_ab() {
    TermAutomatonQuery query = make_query(std::string(kText));
    add_states(query, 2);
    EXPECT_TRUE(query.add_transition(0, 1, "a").has_value());
    query.set_accept(1, true);
    EXPECT_TRUE(query.add_transition(1, 2, "b").has_value());
    query.set_accept(2, true);
    auto finished = query.finish();
    EXPECT_TRUE(finished.has_value()) << (finished ? "" : finished.error().to_string());
    return query;
}

/// The stored `id` of every hit, sorted.
std::vector<std::string> ids_of(IndexSearcher &searcher, const Query &query) {
    auto hits = searcher.search(query, 10);
    EXPECT_TRUE(hits.has_value()) << (hits ? "" : hits.error().to_string());
    std::vector<std::string> ids;
    if (!hits.has_value()) {
        return ids;
    }
    auto stored = searcher.reader().stored_fields();
    EXPECT_TRUE(stored.has_value());
    if (!stored.has_value()) {
        return ids;
    }
    for (const ScoreDoc &hit : hits.value().score_docs) {
        auto doc = stored.value()->document(hit.doc);
        EXPECT_TRUE(doc.has_value());
        if (!doc.has_value()) {
            continue;
        }
        auto values = doc.value().get_values(std::string(kId));
        EXPECT_TRUE(values.has_value());
        if (values.has_value() && !values.value().empty()) {
            ids.push_back(values.value().front());
        }
    }
    std::ranges::sort(ids);
    return ids;
}

/// The global document id of the document whose `id` field is `id`.
int32_t doc_id_of(IndexSearcher &searcher, const std::string &id) {
    auto stored = searcher.reader().stored_fields();
    EXPECT_TRUE(stored.has_value());
    if (!stored.has_value()) {
        return -1;
    }
    auto max_doc = searcher.reader().max_doc();
    EXPECT_TRUE(max_doc.has_value());
    for (int32_t doc = 0; doc < max_doc.value(); ++doc) {
        auto document = stored.value()->document(doc);
        if (!document.has_value()) {
            continue;
        }
        auto values = document.value().get_values(std::string(kId));
        if (values.has_value() && !values.value().empty() && values.value().front() == id) {
            return doc;
        }
    }
    return -1;
}

/// Collects the terms a query reports to its visitors.
class AutomatonTermCollector final : public QueryVisitor {
public:
    [[nodiscard]] VoidResult consume_terms(const Query & /*query*/, std::span<const Term> terms) override {
        for (const Term &term : terms) {
            collected_.push_back(term.text());
        }
        return {};
    }

    [[nodiscard]] const std::vector<std::string> &collected() const noexcept { return collected_; }

private:
    std::vector<std::string> collected_;
};

/// Builds the scorer of the single leaf this test index has.
Result<std::unique_ptr<TermAutomatonScorer>> scorer_of(const TermAutomatonQuery &query, IndexSearcher &searcher,
                                                       ScoreMode score_mode) {
    auto weight = query.create_weight(searcher, score_mode, 1.0F);
    if (!weight.has_value()) {
        return std::unexpected(std::move(weight.error()));
    }
    auto *automaton_weight = dynamic_cast<TermAutomatonWeight *>(weight.value().get());
    if (automaton_weight == nullptr) {
        return err(LuceneError::illegal_state("the query did not build a TermAutomatonWeight"));
    }
    auto leaves = searcher.leaves();
    if (!leaves.has_value()) {
        return std::unexpected(std::move(leaves.error()));
    }
    return automaton_weight->build_scorer(leaves.value().front());
}

} // namespace

TEST(TermAutomatonQueryTest, MatchesTermsInOrder) {
    ByteBuffersDirectory directory;
    build_index(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    IndexSearcher searcher(reader.value());

    // The phrase this automaton stands for, as a reference for the expectations
    // below.  "a c" has a gap in "a b c", and "c b" is in the wrong order
    // wherever it occurs, so neither may match.
    auto phrase = PhraseQuery::create(std::string(kText),
                                      {Term::from_text(std::string(kText), "a"), Term::from_text(std::string(kText), "b")});
    ASSERT_TRUE(phrase.has_value());
    auto phrase_count = searcher.count(phrase.value());
    ASSERT_TRUE(phrase_count.has_value());
    EXPECT_EQ(phrase_count.value(), 2) << "the phrase a b";

    auto gap = PhraseQuery::create(std::string(kText),
                                   {Term::from_text(std::string(kText), "a"), Term::from_text(std::string(kText), "c")});
    ASSERT_TRUE(gap.has_value());
    auto gap_count = searcher.count(gap.value());
    ASSERT_TRUE(gap_count.has_value());
    EXPECT_EQ(gap_count.value(), 1) << "the phrase a c";

    auto reversed = PhraseQuery::create(std::string(kText),
                                        {Term::from_text(std::string(kText), "c"), Term::from_text(std::string(kText), "b")});
    ASSERT_TRUE(reversed.has_value());
    auto reversed_count = searcher.count(reversed.value());
    ASSERT_TRUE(reversed_count.has_value());
    EXPECT_EQ(reversed_count.value(), 0) << "the phrase c b";

    // "a b c" and "a b b c" hold "a" immediately followed by "b"; "b a" holds
    // them in the other order.
    const std::vector<std::string> terms{"a", "b"};
    const TermAutomatonQuery query = in_order(std::string(kText), terms);
    EXPECT_EQ(ids_of(searcher, query), (std::vector<std::string>{"1", "4"}));

    auto count = searcher.count(query);
    ASSERT_TRUE(count.has_value()) << (count ? "" : count.error().to_string());
    EXPECT_EQ(count.value(), 2);
}

TEST(TermAutomatonQueryTest, WildcardMatchesAnyTerm) {
    ByteBuffersDirectory directory;
    build_index(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    // "a", anything, "c": only "a b c" has that shape.
    const TermAutomatonQuery query = a_any_c();
    EXPECT_EQ(query.any_term_id(), 1);

    EXPECT_EQ(ids_of(searcher, query), (std::vector<std::string>{"1"}));
    auto count = searcher.count(query);
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(count.value(), 1);

    // The same, straight from the scorer of the leaf.
    auto scorer = scorer_of(query, searcher, ScoreMode::TopScores);
    ASSERT_TRUE(scorer.has_value()) << (scorer ? "" : scorer.error().to_string());
    ASSERT_NE(scorer.value(), nullptr);
    const int32_t doc_one = doc_id_of(searcher, "1");
    ASSERT_GE(doc_one, 0);
    auto advanced = scorer.value()->advance(doc_one);
    ASSERT_TRUE(advanced.has_value());
    EXPECT_EQ(advanced.value(), doc_one);
    EXPECT_EQ(scorer.value()->freq(), 1);
}

TEST(TermAutomatonQueryTest, CountsEveryMatchInADocument) {
    ByteBuffersDirectory directory;
    build_index(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    // A one-term automaton: how often the term occurs in the document is the
    // frequency the automaton reports.
    const std::vector<std::string> terms{"b"};
    const TermAutomatonQuery query = in_order(std::string(kText), terms);

    auto scorer = scorer_of(query, searcher, ScoreMode::TopScores);
    ASSERT_TRUE(scorer.has_value()) << (scorer ? "" : scorer.error().to_string());
    ASSERT_NE(scorer.value(), nullptr);

    // "a b c": one "b".
    const int32_t doc_one = doc_id_of(searcher, "1");
    ASSERT_GE(doc_one, 0);
    auto first = scorer.value()->advance(doc_one);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first.value(), doc_one);
    EXPECT_EQ(scorer.value()->freq(), 1);

    // "a b b c": two.
    const int32_t doc_four = doc_id_of(searcher, "4");
    ASSERT_GE(doc_four, 0);
    auto second = scorer.value()->advance(doc_four);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second.value(), doc_four);
    EXPECT_EQ(scorer.value()->freq(), 2);

    // The frequency feeds the similarity, so the score is not zero.
    auto score = scorer.value()->score();
    ASSERT_TRUE(score.has_value());
    EXPECT_GT(score.value(), 0.0F);

    // "x y" has no "b", and nothing follows it.
    auto exhausted = scorer.value()->advance(doc_id_of(searcher, "5"));
    ASSERT_TRUE(exhausted.has_value());
    EXPECT_EQ(exhausted.value(), NO_MORE_DOCS);
}

TEST(TermAutomatonQueryTest, ScoresNothingWhenScoresAreNotNeeded) {
    ByteBuffersDirectory directory;
    build_index(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    const std::vector<std::string> terms{"b"};
    const TermAutomatonQuery query = in_order(std::string(kText), terms);

    auto scorer = scorer_of(query, searcher, ScoreMode::CompleteNoScores);
    ASSERT_TRUE(scorer.has_value());
    ASSERT_NE(scorer.value(), nullptr);

    auto advanced = scorer.value()->next_doc();
    ASSERT_TRUE(advanced.has_value());
    ASSERT_NE(advanced.value(), NO_MORE_DOCS);
    auto score = scorer.value()->score();
    ASSERT_TRUE(score.has_value());
    EXPECT_EQ(score.value(), 0.0F);
}

TEST(TermAutomatonQueryTest, NoMatchingTermMatchesNothing) {
    ByteBuffersDirectory directory;
    build_index(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    const std::vector<std::string> terms{"missing", "b"};
    const TermAutomatonQuery query = in_order(std::string(kText), terms);

    auto count = searcher.count(query);
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(count.value(), 0);
}

TEST(TermAutomatonQueryTest, RewritesToTheSimplestQueryThatMatches) {
    ByteBuffersDirectory directory;
    build_index(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    // An automaton that accepts nothing becomes MatchNoDocsQuery.
    TermAutomatonQuery accepts_nothing = make_query(std::string(kText));
    ASSERT_TRUE(accepts_nothing.finish().has_value());
    auto empty = accepts_nothing.rewrite(searcher);
    ASSERT_TRUE(empty.has_value()) << (empty ? "" : empty.error().to_string());
    ASSERT_NE(empty.value(), nullptr);
    EXPECT_NE(dynamic_cast<MatchNoDocsQuery *>(empty.value().get()), nullptr);

    // A single term becomes a TermQuery.
    TermAutomatonQuery single = make_query(std::string(kText));
    add_states(single, 1);
    ASSERT_TRUE(single.add_transition(0, 1, "a").has_value());
    single.set_accept(1, true);
    ASSERT_TRUE(single.finish().has_value());
    auto term_query = single.rewrite(searcher);
    ASSERT_TRUE(term_query.has_value());
    auto *as_term = dynamic_cast<TermQuery *>(term_query.value().get());
    ASSERT_NE(as_term, nullptr);
    EXPECT_EQ(as_term->term().text(), "a");

    // A "sausage" with one term per position becomes a PhraseQuery.
    const std::vector<std::string> terms{"a", "b"};
    const TermAutomatonQuery phrase = in_order(std::string(kText), terms);
    auto as_phrase = phrase.rewrite(searcher);
    ASSERT_TRUE(as_phrase.has_value());
    EXPECT_NE(dynamic_cast<PhraseQuery *>(as_phrase.value().get()), nullptr);

    // Two alternatives at the same position cannot be a PhraseQuery, but they fit
    // a MultiPhraseQuery.
    TermAutomatonQuery multi = make_query(std::string(kText));
    add_states(multi, 1);
    ASSERT_TRUE(multi.add_transition(0, 1, "a").has_value());
    ASSERT_TRUE(multi.add_transition(0, 1, "b").has_value());
    multi.set_accept(1, true);
    ASSERT_TRUE(multi.finish().has_value());
    auto as_multi = multi.rewrite(searcher);
    ASSERT_TRUE(as_multi.has_value());
    EXPECT_NE(dynamic_cast<MultiPhraseQuery *>(as_multi.value().get()), nullptr);

    // An accepting state with outgoing transitions is no sausage at all, so the
    // query is left alone.
    const TermAutomatonQuery as_is = a_or_ab();
    auto unchanged = as_is.rewrite(searcher);
    ASSERT_TRUE(unchanged.has_value());
    EXPECT_EQ(unchanged.value(), nullptr);
}

TEST(TermAutomatonQueryTest, RejectsAutomataTheReferenceRejects) {
    // Accepting the empty string would match every document.
    TermAutomatonQuery empty_accepted = make_query(std::string(kText));
    empty_accepted.set_accept(0, true);
    auto empty_finished = empty_accepted.finish();
    EXPECT_FALSE(empty_finished.has_value());

    // A leading wildcard matches every term at position 0, i.e. any document.
    TermAutomatonQuery leading_any = make_query(std::string(kText));
    add_states(leading_any, 1);
    ASSERT_TRUE(leading_any.add_any_transition(0, 1).has_value());
    leading_any.set_accept(1, true);
    EXPECT_FALSE(leading_any.finish().has_value());

    // A trailing wildcard (one that leads into an accepting state) matches any
    // term at the end, i.e. any document.
    TermAutomatonQuery trailing_any = make_query(std::string(kText));
    add_states(trailing_any, 2);
    ASSERT_TRUE(trailing_any.add_transition(0, 1, "a").has_value());
    ASSERT_TRUE(trailing_any.add_any_transition(1, 2).has_value());
    trailing_any.set_accept(2, true);
    EXPECT_FALSE(trailing_any.finish().has_value());

    // The weight refuses a query that was never compiled.
    ByteBuffersDirectory directory;
    build_index(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());
    const TermAutomatonQuery unfinished = make_query(std::string(kText));
    auto weight = unfinished.create_weight(searcher, ScoreMode::TopScores, 1.0F);
    EXPECT_FALSE(weight.has_value());
}

TEST(TermAutomatonQueryTest, ReportsItsTermsToVisitors) {
    ByteBuffersDirectory directory;
    build_index(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    const std::vector<std::string> terms{"a", "b"};
    const TermAutomatonQuery query = in_order(std::string(kText), terms);

    AutomatonTermCollector collector;
    ASSERT_TRUE(query.visit(collector).has_value());
    auto collected = collector.collected();
    std::ranges::sort(collected);
    EXPECT_EQ(collected, (std::vector<std::string>{"a", "b"}));

    // The wildcard has no term, so it is not reported.
    const TermAutomatonQuery wildcard = a_any_c();
    AutomatonTermCollector wildcard_collector;
    ASSERT_TRUE(wildcard.visit(wildcard_collector).has_value());
    auto wildcard_terms = wildcard_collector.collected();
    std::ranges::sort(wildcard_terms);
    EXPECT_EQ(wildcard_terms, (std::vector<std::string>{"a", "c"}));
}

TEST(TermAutomatonQueryTest, ExplainsAndRendersItself) {
    ByteBuffersDirectory directory;
    build_index(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    // "a" or "a b": every document holding an "a" matches.
    const TermAutomatonQuery query = a_or_ab();

    const int32_t matching = doc_id_of(searcher, "1");
    ASSERT_GE(matching, 0);
    auto explanation = searcher.explain(query, matching);
    ASSERT_TRUE(explanation.has_value()) << (explanation ? "" : explanation.error().to_string());
    EXPECT_TRUE(explanation.value().is_match());
    const std::string rendered = explanation.value().to_string();
    EXPECT_NE(rendered.find("score for term: a"), std::string::npos) << rendered;

    const int32_t not_matching = doc_id_of(searcher, "5");
    ASSERT_GE(not_matching, 0);
    auto no_match = searcher.explain(query, not_matching);
    ASSERT_TRUE(no_match.has_value());
    EXPECT_FALSE(no_match.value().is_match());

    auto text = query.to_string(kText);
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text.value().find("TermAutomatonQuery(field=text"), 0U) << text.value();

    auto dot = query.to_dot();
    ASSERT_TRUE(dot.has_value()) << (dot ? "" : dot.error().to_string());
    EXPECT_NE(dot.value().find("digraph Automaton"), std::string::npos);
    EXPECT_NE(dot.value().find("label=\"b\""), std::string::npos);

    // The wildcard is rendered as `*`.
    auto wildcard_dot = a_any_c().to_dot();
    ASSERT_TRUE(wildcard_dot.has_value());
    EXPECT_NE(wildcard_dot.value().find("label=\"*\""), std::string::npos);
}

TEST(TermAutomatonQueryTest, ComparesByIdentity) {
    const std::vector<std::string> terms{"a", "b"};
    const TermAutomatonQuery first = in_order(std::string(kText), terms);
    const TermAutomatonQuery second = in_order(std::string(kText), terms);

    EXPECT_TRUE(first.equals(first));
    EXPECT_FALSE(first.equals(second));
    EXPECT_EQ(first.hash(), first.hash());
}
