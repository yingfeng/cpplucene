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

// Walking a query with a visitor: the automaton queries report the terms they
// match through `consume_terms` (one term) or `consume_terms_matching` (a class of
// terms), and a sub visitor for a `MUST_NOT` clause reports nothing at all.

module;

#include <gtest/gtest.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.query_visitor;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kField = "f";

/// A visitor that records what it was told.
class RecordingVisitor : public QueryVisitor {
public:
    [[nodiscard]] VoidResult consume_terms(const Query & /*query*/, std::span<const Term> terms) override {
        ++term_calls;
        for (const Term &term : terms) {
            terms_seen.push_back(term.text());
        }
        return {};
    }

    [[nodiscard]] VoidResult consume_terms_matching(const Query & /*query*/, std::string_view field,
                                                    const AutomatonSupplier &automaton) override {
        ++matching_calls;
        matching_field = std::string(field);
        auto built = automaton();
        if (!built.has_value()) {
            return std::unexpected(std::move(built.error()));
        }
        matching_automaton = std::move(built.value());
        return {};
    }

    [[nodiscard]] VoidResult visit_leaf(const Query & /*query*/) override {
        ++leaf_calls;
        return {};
    }

    [[nodiscard]] bool accept_field(std::string_view field) const override {
        return accept_all || field == interesting_field;
    }

    std::vector<std::string> terms_seen;
    int32_t term_calls{0};
    int32_t matching_calls{0};
    int32_t leaf_calls{0};
    std::string matching_field;
    std::optional<ByteRunAutomaton> matching_automaton;
    bool accept_all{true};
    std::string interesting_field{std::string(kField)};
};

/// Whether `automaton` accepts `text`; it runs over bytes, which is what a term
/// holds.
bool accepts(ByteRunAutomaton &automaton, std::string_view text) {
    const auto *bytes = reinterpret_cast<const uint8_t *>(text.data());
    auto accepted = automaton.run(std::span<const uint8_t>(bytes, text.size()), 0, text.size());
    EXPECT_TRUE(accepted.has_value()) << (accepted.has_value() ? "" : accepted.error().to_string());
    return accepted.has_value() && accepted.value();
}

TermQuery term_query(std::string_view text) {
    return TermQuery(Term::from_text(std::string(kField), text));
}

} // namespace

TEST(QueryVisitorTest, TheDefaultConsumeTermsMatchingFallsBackToVisitLeaf) {
    // A query that reports a class of terms through the query visitor's default
    // implementation is reported as a leaf query.
    class Plain final : public Query {
    public:
        [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher & /*searcher*/,
                                                                    ScoreMode /*score_mode*/,
                                                                    float /*boost*/) const override {
            return err(LuceneError::unsupported_operation("not searched in this test"));
        }
        [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override {
            return visitor.consume_terms_matching(*this, kField, []() -> Result<std::optional<ByteRunAutomaton>> {
                return std::optional<ByteRunAutomaton>{};
            });
        }
        [[nodiscard]] Result<std::string> to_string(std::string_view /*field*/) const override {
            return std::string("Plain");
        }
        [[nodiscard]] bool equals(const Query &other) const override { return dynamic_cast<const Plain *>(&other) != nullptr; }
        [[nodiscard]] size_t hash() const override { return 1; }
    };

    class LeafVisitor final : public QueryVisitor {
    public:
        [[nodiscard]] VoidResult visit_leaf(const Query & /*query*/) override {
            ++leaf_calls;
            return {};
        }
        int32_t leaf_calls{0};
    };

    const Plain query;
    LeafVisitor visitor;
    ASSERT_TRUE(query.visit(visitor).has_value());
    EXPECT_EQ(visitor.leaf_calls, 1);
}

TEST(QueryVisitorTest, ASubVisitorOfAMustNotClauseReportsNothing) {
    RecordingVisitor visitor;
    const TermQuery query = term_query("abc");

    DefaultQueryVisitor should = visitor.get_sub_visitor(Occur::Should, query);
    EXPECT_FALSE(should.is_empty());
    ASSERT_TRUE(should.consume_terms(query, std::vector<Term>{Term::from_text(std::string(kField), "abc")}).has_value());
    EXPECT_EQ(visitor.term_calls, 1);

    DefaultQueryVisitor must_not = visitor.get_sub_visitor(Occur::MustNot, query);
    EXPECT_TRUE(must_not.is_empty());
    EXPECT_FALSE(must_not.accept_field(kField));
    ASSERT_TRUE(must_not.consume_terms(query, std::vector<Term>{Term::from_text(std::string(kField), "abc")}).has_value());
    ASSERT_TRUE(must_not.visit_leaf(query).has_value());
    // Nothing reached the parent.
    EXPECT_EQ(visitor.term_calls, 1);
    EXPECT_EQ(visitor.leaf_calls, 0);
    // A nested clause stays empty.
    EXPECT_TRUE(must_not.get_sub_visitor(Occur::Should, query).is_empty());
}

TEST(QueryVisitorTest, ASingleTermAutomatonReportsOneTerm) {
    auto automaton = Automata::make_string("abc");
    ASSERT_TRUE(automaton.has_value()) << automaton.error().to_string();
    auto query = AutomatonQuery::from_automaton(Term::from_text(std::string(kField), "abc"), std::move(automaton.value()));
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    ASSERT_EQ(query.value().compiled().type(), AutomatonType::kSingle);

    RecordingVisitor visitor;
    ASSERT_TRUE(query.value().visit(visitor).has_value());
    EXPECT_EQ(visitor.term_calls, 1);
    EXPECT_EQ(visitor.matching_calls, 0);
    ASSERT_EQ(visitor.terms_seen.size(), 1U);
    EXPECT_EQ(visitor.terms_seen.front(), "abc");
}

TEST(QueryVisitorTest, ANormalAutomatonReportsATermMatcher) {
    // "a" followed by anything.
    auto first = Automata::make_char('a');
    ASSERT_TRUE(first.has_value()) << first.error().to_string();
    auto any = Automata::make_any_string();
    ASSERT_TRUE(any.has_value()) << any.error().to_string();
    auto concatenated = Operations::concatenate(std::move(first.value()), std::move(any.value()));
    ASSERT_TRUE(concatenated.has_value()) << concatenated.error().to_string();
    auto query = AutomatonQuery::from_automaton(Term::from_text(std::string(kField), "a*"),
                                                std::move(concatenated.value()));
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    ASSERT_EQ(query.value().compiled().type(), AutomatonType::kNormal);

    RecordingVisitor visitor;
    ASSERT_TRUE(query.value().visit(visitor).has_value());
    EXPECT_EQ(visitor.term_calls, 0);
    EXPECT_EQ(visitor.matching_calls, 1);
    EXPECT_EQ(visitor.matching_field, std::string(kField));
    ASSERT_TRUE(visitor.matching_automaton.has_value());
    EXPECT_TRUE(accepts(visitor.matching_automaton.value(), "a"));
    EXPECT_TRUE(accepts(visitor.matching_automaton.value(), "abc"));
    EXPECT_FALSE(accepts(visitor.matching_automaton.value(), "b"));
}

TEST(QueryVisitorTest, AnAutomatonThatAcceptsEverythingReportsAnAnyStringMatcher) {
    auto automaton = Automata::make_any_string();
    ASSERT_TRUE(automaton.has_value()) << automaton.error().to_string();
    auto query = AutomatonQuery::from_automaton(Term::from_text(std::string(kField), "*"), std::move(automaton.value()));
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    ASSERT_EQ(query.value().compiled().type(), AutomatonType::kAll);

    RecordingVisitor visitor;
    ASSERT_TRUE(query.value().visit(visitor).has_value());
    EXPECT_EQ(visitor.matching_calls, 1);
    ASSERT_TRUE(visitor.matching_automaton.has_value());
    EXPECT_TRUE(accepts(visitor.matching_automaton.value(), ""));
    EXPECT_TRUE(accepts(visitor.matching_automaton.value(), "anything"));
}

TEST(QueryVisitorTest, AnAutomatonThatAcceptsNothingReportsNothing) {
    auto automaton = Automata::make_empty();
    ASSERT_TRUE(automaton.has_value()) << automaton.error().to_string();
    auto query = AutomatonQuery::from_automaton(Term::from_text(std::string(kField), "nothing"),
                                                std::move(automaton.value()));
    ASSERT_TRUE(query.has_value()) << query.error().to_string();

    RecordingVisitor visitor;
    ASSERT_TRUE(query.value().visit(visitor).has_value());
    EXPECT_EQ(visitor.term_calls, 0);
    EXPECT_EQ(visitor.matching_calls, 0);
    EXPECT_EQ(visitor.leaf_calls, 0);
}

TEST(QueryVisitorTest, AnUninterestingFieldIsNotReported) {
    auto automaton = Automata::make_string("abc");
    ASSERT_TRUE(automaton.has_value());
    auto query = AutomatonQuery::from_automaton(Term::from_text(std::string(kField), "abc"), std::move(automaton.value()));
    ASSERT_TRUE(query.has_value()) << query.error().to_string();

    RecordingVisitor visitor;
    visitor.accept_all = false;
    visitor.interesting_field = "other";
    ASSERT_TRUE(query.value().visit(visitor).has_value());
    EXPECT_EQ(visitor.term_calls, 0);
    EXPECT_EQ(visitor.matching_calls, 0);
}

TEST(QueryVisitorTest, BuiltInSortFieldsHaveNothingToRewrite) {
    ByteBuffersDirectory directory;
    {
        WhitespaceAnalyzer analyzer;
        IndexWriterConfig config(analyzer);
        auto writer = IndexWriter::open(directory, config);
        ASSERT_TRUE(writer.has_value());
        ASSERT_TRUE(writer.value()->commit().has_value());
        ASSERT_TRUE(writer.value()->close().has_value());
    }
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    const IndexSearcher searcher(reader.value());

    auto num = SortField::create("num", SortFieldType::kLong);
    ASSERT_TRUE(num.has_value());
    auto rewritten_field = num.value().rewrite(searcher);
    ASSERT_TRUE(rewritten_field.has_value());
    EXPECT_FALSE(rewritten_field.value().has_value());

    auto sort = Sort::with_fields({num.value()});
    ASSERT_TRUE(sort.has_value());
    auto rewritten_sort = sort.value().rewrite(searcher);
    ASSERT_TRUE(rewritten_sort.has_value());
    EXPECT_FALSE(rewritten_sort.value().has_value());

    auto relevance = Sort::relevance();
    ASSERT_TRUE(relevance.has_value());
    auto rewritten_relevance = relevance.value().rewrite(searcher);
    ASSERT_TRUE(rewritten_relevance.has_value());
    EXPECT_FALSE(rewritten_relevance.value().has_value());
}
