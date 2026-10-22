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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

module lucene_core:ut.compiled_automaton;

import lucene_core;

using namespace lucene;

namespace {

std::vector<BytesRef> to_terms(std::initializer_list<std::string_view> texts) {
    std::vector<BytesRef> terms;
    terms.reserve(texts.size());
    for (const auto text : texts) {
        terms.push_back(BytesRef::from_string(text));
    }
    return terms;
}

/// Mirrors rlucene's `build` test helper: a string union compiled into the
/// "normal" form so that [`CompiledAutomaton::floor`] can be exercised.
CompiledAutomaton build(const std::vector<std::string> &strings) {
    std::vector<BytesRef> terms;
    terms.reserve(strings.size());
    for (const auto &text : strings) {
        terms.push_back(BytesRef::from_string(text));
    }
    std::ranges::sort(terms);

    auto automaton = Automata::make_string_union(terms);
    EXPECT_TRUE(automaton.has_value());
    auto compiled = CompiledAutomaton::with_binary(std::move(automaton.value()), true, false, false);
    EXPECT_TRUE(compiled.has_value());
    return std::move(compiled.value());
}

void expect_floor(CompiledAutomaton &compiled, std::string_view input,
                  const std::optional<std::string_view> &expected) {
    BytesRef key = BytesRef::from_string(input);
    BytesRefBuilder builder;

    auto result = compiled.floor(key, builder);
    ASSERT_TRUE(result.has_value()) << "input=" << input;
    if (!expected.has_value()) {
        EXPECT_FALSE(result->has_value()) << "input=" << input;
        return;
    }
    ASSERT_TRUE(result->has_value()) << "input=" << input;
    EXPECT_EQ(**result, BytesRef::from_string(*expected)) << "input=" << input;
}

/// Runs `text` through the compiled automaton.
bool accepts(CompiledAutomaton &compiled, std::string_view text) {
    auto automaton = compiled.get_automaton();
    EXPECT_TRUE(automaton.has_value());
    BytesRef bytes = BytesRef::from_string(text);
    auto result = automaton->run(bytes.as_slice(), bytes.offset(), bytes.length());
    EXPECT_TRUE(result.has_value());
    return result.value();
}

} // namespace

TEST(CompiledAutomatonTest, EmptyAutomatonBecomesNone) {
    // An automaton with no states at all is normalized into "accepts nothing".
    auto compiled = CompiledAutomaton::from_automaton(Automaton{});
    ASSERT_TRUE(compiled.has_value());

    EXPECT_EQ(compiled->type(), AutomatonType::kNone);
    EXPECT_TRUE(compiled->finite());
    EXPECT_EQ(compiled->sink_state(), -1);
    EXPECT_FALSE(compiled->term().has_value());
    EXPECT_FALSE(compiled->run_automaton().has_value());
    EXPECT_FALSE(compiled->nfa_run_automaton().has_value());
    EXPECT_FALSE(compiled->get_automaton().has_value());

    auto empty = CompiledAutomaton::from_automaton(Automata::make_empty().value());
    ASSERT_TRUE(empty.has_value());
    EXPECT_EQ(empty->type(), AutomatonType::kNone);
}

TEST(CompiledAutomatonTest, BinaryAll) {
    // LUCENE-6367
    Automaton automaton;
    const int32_t state = automaton.create_state();
    automaton.set_accept(state, true);
    ASSERT_TRUE(automaton.add_transition(state, state, 0, 0xff).has_value());
    ASSERT_TRUE(automaton.finish_state().has_value());

    auto compiled = CompiledAutomaton::with_binary(std::move(automaton), false, true, true);
    ASSERT_TRUE(compiled.has_value());
    EXPECT_EQ(compiled->type(), AutomatonType::kAll);
    EXPECT_TRUE(compiled->finite());
}

TEST(CompiledAutomatonTest, UnicodeAll) {
    // LUCENE-6367
    Automaton automaton;
    const int32_t state = automaton.create_state();
    automaton.set_accept(state, true);
    ASSERT_TRUE(automaton.add_transition(state, state, 0, 0x10ffff).has_value());
    ASSERT_TRUE(automaton.finish_state().has_value());

    auto compiled = CompiledAutomaton::with_binary(std::move(automaton), false, true, false);
    ASSERT_TRUE(compiled.has_value());
    EXPECT_EQ(compiled->type(), AutomatonType::kAll);
}

TEST(CompiledAutomatonTest, AnyStringIsAll) {
    auto compiled = CompiledAutomaton::from_automaton(Automata::make_any_string().value());
    ASSERT_TRUE(compiled.has_value());
    EXPECT_EQ(compiled->type(), AutomatonType::kAll);
    EXPECT_EQ(compiled->sink_state(), -1);
}

TEST(CompiledAutomatonTest, BinarySingleton) {
    // LUCENE-6367
    auto automaton = Automata::make_string("foobar");
    ASSERT_TRUE(automaton.has_value());
    auto compiled = CompiledAutomaton::with_binary(std::move(automaton.value()), true, true, true);
    ASSERT_TRUE(compiled.has_value());
    EXPECT_EQ(compiled->type(), AutomatonType::kSingle);
    ASSERT_TRUE(compiled->term().has_value());
    EXPECT_EQ(*compiled->term(), BytesRef::from_string("foobar"));
}

TEST(CompiledAutomatonTest, UnicodeSingleton) {
    // LUCENE-6367
    auto automaton = Automata::make_string("caf\u00e9");
    ASSERT_TRUE(automaton.has_value());
    auto compiled = CompiledAutomaton::with_binary(std::move(automaton.value()), true, true, false);
    ASSERT_TRUE(compiled.has_value());
    EXPECT_EQ(compiled->type(), AutomatonType::kSingle);
    ASSERT_TRUE(compiled->term().has_value());
    // The singleton term is stored as UTF-8.
    EXPECT_EQ(*compiled->term(), BytesRef::from_string("caf\u00e9"));
    EXPECT_EQ(compiled->term()->length(), 5u);
}

TEST(CompiledAutomatonTest, SimplifyDisabledKeepsNormal) {
    auto automaton = Automata::make_string("foo");
    ASSERT_TRUE(automaton.has_value());

    auto compiled = CompiledAutomaton::create(std::move(automaton.value()), true, false);
    ASSERT_TRUE(compiled.has_value());
    EXPECT_EQ(compiled->type(), AutomatonType::kNormal);
    EXPECT_TRUE(compiled->finite());
    ASSERT_TRUE(compiled->run_automaton().has_value());
    EXPECT_FALSE(compiled->nfa_run_automaton().has_value());
    EXPECT_FALSE(compiled->term().has_value());

    auto automaton_enum = compiled->get_automaton();
    ASSERT_TRUE(automaton_enum.has_value());
    EXPECT_TRUE(automaton_enum->is_byte());
    EXPECT_GT(automaton_enum->get_size(), 0u);

    BytesRef text = BytesRef::from_string("foo");
    auto run = automaton_enum->run(text.as_slice(), 0, text.length());
    ASSERT_TRUE(run.has_value());
    EXPECT_TRUE(run.value());
    BytesRef other = BytesRef::from_string("bar");
    auto run_other = automaton_enum->run(other.as_slice(), 0, other.length());
    ASSERT_TRUE(run_other.has_value());
    EXPECT_FALSE(run_other.value());
}

TEST(CompiledAutomatonTest, DeterministicUnionAcceptsItsTerms) {
    auto compiled = CompiledAutomaton::from_automaton(
            Automata::make_string_union(to_terms({"bar", "baz"})).value());
    ASSERT_TRUE(compiled.has_value());
    EXPECT_EQ(compiled->type(), AutomatonType::kNormal);
    EXPECT_FALSE(compiled->finite());
    ASSERT_TRUE(compiled->run_automaton().has_value());
    EXPECT_FALSE(compiled->nfa_run_automaton().has_value());
    EXPECT_EQ(compiled->sink_state(), -1);

    EXPECT_TRUE(accepts(*compiled, "bar"));
    EXPECT_TRUE(accepts(*compiled, "baz"));
    EXPECT_FALSE(accepts(*compiled, "ba"));
    EXPECT_FALSE(accepts(*compiled, "barz"));
}

TEST(CompiledAutomatonTest, NonDeterministicAutomatonUsesNfa) {
    auto first = Automata::make_string("ab");
    auto second = Automata::make_string("ac");
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());

    auto automaton = Operations::union_with(first.value(), second.value());
    ASSERT_TRUE(automaton.has_value());
    ASSERT_FALSE(automaton->is_deterministic());

    auto compiled = CompiledAutomaton::with_binary(std::move(automaton.value()), false, true, false);
    ASSERT_TRUE(compiled.has_value());
    EXPECT_EQ(compiled->type(), AutomatonType::kNormal);
    // The UTF-8 conversion is still non-deterministic, so the NFA path is used.
    ASSERT_TRUE(compiled->nfa_run_automaton().has_value());
    EXPECT_FALSE(compiled->run_automaton().has_value());
    EXPECT_EQ(compiled->sink_state(), -1);

    auto automaton_enum = compiled->get_automaton();
    ASSERT_TRUE(automaton_enum.has_value());
    EXPECT_FALSE(automaton_enum->is_byte());
    BytesRef text = BytesRef::from_string("ab");
    auto run = automaton_enum->run(text.as_slice(), 0, text.length());
    ASSERT_TRUE(run.has_value());
    EXPECT_TRUE(run.value());
}

TEST(CompiledAutomatonTest, SinkStateAndFiniteFlag) {
    // A binary automaton whose initial state accepts every suffix.
    Automaton automaton;
    const int32_t state = automaton.create_state();
    automaton.set_accept(state, true);
    ASSERT_TRUE(automaton.add_transition(state, state, 0, 0xff).has_value());
    ASSERT_TRUE(automaton.finish_state().has_value());

    auto compiled = CompiledAutomaton::with_binary(std::move(automaton), true, false, true);
    ASSERT_TRUE(compiled.has_value());
    EXPECT_EQ(compiled->type(), AutomatonType::kNormal);
    EXPECT_TRUE(compiled->finite());
    EXPECT_EQ(compiled->sink_state(), 0);
}

TEST(CompiledAutomatonTest, CommonSuffixOfLeadingWildcard) {
    auto any = Automata::make_any_string();
    auto suffix = Automata::make_string("bar");
    ASSERT_TRUE(any.has_value());
    ASSERT_TRUE(suffix.has_value());

    auto automaton = Operations::concatenate(any.value(), suffix.value());
    ASSERT_TRUE(automaton.has_value());

    // `finite` is false, so the common suffix optimization runs.
    auto compiled = CompiledAutomaton::with_binary(std::move(automaton.value()), false, false, false);
    ASSERT_TRUE(compiled.has_value());
    ASSERT_TRUE(compiled->common_suffix_ref().has_value());
    EXPECT_EQ(*compiled->common_suffix_ref(), BytesRef::from_string("bar"));

    EXPECT_TRUE(accepts(*compiled, "xxbar"));
    EXPECT_FALSE(accepts(*compiled, "xxbaz"));
}

TEST(CompiledAutomatonTest, FiniteLanguageHasNoCommonSuffix) {
    // `make_string_union` expects the terms in sorted order.
    auto automaton = Automata::make_string_union(to_terms({"bazbar", "foobar"}));
    ASSERT_TRUE(automaton.has_value());

    auto compiled = CompiledAutomaton::with_binary(std::move(automaton.value()), true, false, false);
    ASSERT_TRUE(compiled.has_value());
    // Finite languages skip the (expensive) common suffix computation.
    EXPECT_FALSE(compiled->common_suffix_ref().has_value());
}

TEST(CompiledAutomatonTest, FloorBasic) {
    auto compiled = build({"fob", "foo", "goo"});

    expect_floor(compiled, "goo", "goo");
    expect_floor(compiled, "ga", "foo");
    expect_floor(compiled, "g", "foo");
    expect_floor(compiled, "foc", "fob");
    expect_floor(compiled, "foz", "foo");
    expect_floor(compiled, "f", std::nullopt);
    expect_floor(compiled, "", std::nullopt);
    expect_floor(compiled, "aa", std::nullopt);
    expect_floor(compiled, "zzz", "goo");
}

TEST(CompiledAutomatonTest, FloorBruteForceAgainstSortedTerms) {
    const std::vector<std::string> terms{"bar", "baz", "foo"};
    auto compiled = build(terms);

    std::vector<BytesRef> sorted;
    for (const auto &text : terms) {
        sorted.push_back(BytesRef::from_string(text));
    }
    std::ranges::sort(sorted);

    // Every prefix of every term, plus a few strings that sort between them.
    const std::vector<std::string> probes{"", "a", "b", "ba", "bar", "barz", "baz", "boo",
                                          "f", "fo", "foo", "fooo", "foz", "zzz"};
    for (const auto &probe : probes) {
        BytesRef key = BytesRef::from_string(probe);
        const auto insert = std::lower_bound(sorted.begin(), sorted.end(), key);
        const bool exact = insert != sorted.end() && *insert == key;

        std::optional<BytesRef> expected;
        if (exact) {
            expected = *insert;
        } else if (insert != sorted.begin()) {
            expected = *(insert - 1);
        }

        BytesRefBuilder builder;
        auto result = compiled.floor(key, builder);
        ASSERT_TRUE(result.has_value()) << "probe=" << probe;
        if (!expected.has_value()) {
            EXPECT_FALSE(result->has_value()) << "probe=" << probe;
            continue;
        }
        ASSERT_TRUE(result->has_value()) << "probe=" << probe;
        EXPECT_EQ(**result, *expected) << "probe=" << probe;
    }
}

TEST(CompiledAutomatonTest, FloorRejectsNonNormalAutomata) {
    auto compiled = CompiledAutomaton::from_automaton(Automata::make_empty().value());
    ASSERT_TRUE(compiled.has_value());

    BytesRefBuilder builder;
    auto result = compiled->floor(BytesRef::from_string("anything"), builder);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind(), ErrorKind::kIllegalState);
}

TEST(CompiledAutomatonTest, Equality) {
    auto left = CompiledAutomaton::from_automaton(
            Automata::make_string_union(to_terms({"bar", "baz"})).value());
    auto right = CompiledAutomaton::from_automaton(
            Automata::make_string_union(to_terms({"bar", "baz"})).value());
    ASSERT_TRUE(left.has_value());
    ASSERT_TRUE(right.has_value());
    EXPECT_TRUE(*left == *right);

    auto other = CompiledAutomaton::from_automaton(Automata::make_string("foo").value());
    ASSERT_TRUE(other.has_value());
    EXPECT_TRUE(*left != *other);

    auto single_a = CompiledAutomaton::create(Automata::make_string("foo").value(), true, true);
    auto single_b = CompiledAutomaton::create(Automata::make_string("foo").value(), true, true);
    auto single_c = CompiledAutomaton::create(Automata::make_string("bar").value(), true, true);
    ASSERT_TRUE(single_a.has_value());
    ASSERT_TRUE(single_b.has_value());
    ASSERT_TRUE(single_c.has_value());
    EXPECT_EQ(single_a->type(), AutomatonType::kSingle);
    EXPECT_TRUE(*single_a == *single_b);
    EXPECT_TRUE(*single_a != *single_c);

    auto none_a = CompiledAutomaton::from_automaton(Automata::make_empty().value());
    auto none_b = CompiledAutomaton::from_automaton(Automata::make_empty().value());
    ASSERT_TRUE(none_a.has_value());
    ASSERT_TRUE(none_b.has_value());
    EXPECT_TRUE(*none_a == *none_b);
}
