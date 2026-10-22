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
#include <string>
#include <vector>

module lucene_core:ut.nfa_run_automaton;

import lucene_core;

using namespace lucene;

namespace {

constexpr size_t WORK_LIMIT = Operations::DEFAULT_DETERMINIZE_WORK_LIMIT;

std::vector<int32_t> code_points(std::string_view text) {
    auto decoded = StringHelper::utf8_to_code_points(text);
    EXPECT_TRUE(decoded.has_value());
    return decoded.has_value() ? std::move(decoded.value()) : std::vector<int32_t>{};
}

/// Whether the reference (determinized) automaton accepts `text`.
bool reference_accepts(const Automaton &a, std::string_view text) {
    auto deterministic = Operations::determinize(a, WORK_LIMIT);
    const Automaton &det = deterministic.has_value() && deterministic->has_value() ? deterministic->value() : a;
    return Operations::run_str(det, text);
}

} // namespace

TEST(NFARunAutomatonTest, AcceptsDeterministicAutomaton) {
    auto automaton = Automata::make_string("foo");
    ASSERT_TRUE(automaton.has_value());
    ASSERT_TRUE(automaton->is_deterministic());

    auto runnable = NFARunAutomaton::create(std::move(automaton.value()));
    ASSERT_TRUE(runnable.has_value());

    EXPECT_TRUE(runnable->run_codepoints(code_points("foo")).value());
    EXPECT_FALSE(runnable->run_codepoints(code_points("bar")).value());
    EXPECT_FALSE(runnable->run_codepoints(code_points("fo")).value());
    EXPECT_FALSE(runnable->run_codepoints(code_points("fooo")).value());
    EXPECT_TRUE(runnable->run_codepoints(std::vector<int32_t>{}).value() == false);
}

TEST(NFARunAutomatonTest, AcceptsNonDeterministicAutomaton) {
    // The union introduces epsilon transitions, so the result is an NFA.
    auto foo = Automata::make_string("foo");
    auto foobar = Automata::make_string("foobar");
    ASSERT_TRUE(foo.has_value());
    ASSERT_TRUE(foobar.has_value());

    auto united = Operations::union_with(foo.value(), foobar.value());
    ASSERT_TRUE(united.has_value());
    ASSERT_FALSE(united->is_deterministic());

    auto runnable = NFARunAutomaton::create(std::move(united.value()));
    ASSERT_TRUE(runnable.has_value());

    for (const std::string_view text : {"foo", "foobar"}) {
        EXPECT_TRUE(runnable->run_codepoints(code_points(text)).value()) << text;
    }
    for (const std::string_view text : {"", "fo", "fooba", "foobarbaz", "bar"}) {
        EXPECT_FALSE(runnable->run_codepoints(code_points(text)).value()) << text;
    }
}

TEST(NFARunAutomatonTest, ReversedAutomaton) {
    // Reversing a union has one epsilon per accept state, so the result is an
    // NFA and the runnable has to determinize on the fly.
    auto foo = Automata::make_string("foo");
    auto bar = Automata::make_string("bar");
    ASSERT_TRUE(foo.has_value());
    ASSERT_TRUE(bar.has_value());
    auto united = Operations::union_with(foo.value(), bar.value());
    ASSERT_TRUE(united.has_value());
    auto reversed = Operations::reverse(united.value());
    ASSERT_TRUE(reversed.has_value());

    auto runnable = NFARunAutomaton::create(std::move(reversed.value()));
    ASSERT_TRUE(runnable.has_value());

    EXPECT_TRUE(runnable->run_codepoints(code_points("oof")).value());
    EXPECT_TRUE(runnable->run_codepoints(code_points("rab")).value());
    EXPECT_FALSE(runnable->run_codepoints(code_points("foo")).value());
}

TEST(NFARunAutomatonTest, MatchesTheDeterminizedAutomaton) {
    // A small regular expression, checked against the eager determinization.
    auto regexp = RegExp::from_string("(foo|bar)+");
    ASSERT_TRUE(regexp.has_value());
    auto automaton = regexp->to_automaton();
    ASSERT_TRUE(automaton.has_value());

    auto runnable = NFARunAutomaton::create(automaton.value());
    ASSERT_TRUE(runnable.has_value());

    for (const std::string_view text : {"", "foo", "bar", "foobar", "barfoo", "foofoo", "barbar", "foob",
                                        "xfoo", "foox", "barfoo bar"}) {
        EXPECT_EQ(runnable->run_codepoints(code_points(text)).value(), reference_accepts(automaton.value(), text))
                << text;
    }
}

TEST(NFARunAutomatonTest, StepAndIsAccept) {
    auto automaton = Automata::make_string("ab");
    ASSERT_TRUE(automaton.has_value());
    auto runnable = NFARunAutomaton::create(std::move(automaton.value()));
    ASSERT_TRUE(runnable.has_value());

    auto inital_accept = runnable->is_accept(0);
    ASSERT_TRUE(inital_accept.has_value());
    EXPECT_FALSE(inital_accept.value());

    auto state = runnable->step(0, 'a');
    ASSERT_TRUE(state.has_value());
    ASSERT_GE(state.value(), 0);
    EXPECT_FALSE(runnable->is_accept(state.value()).value());

    auto final_state = runnable->step(state.value(), 'b');
    ASSERT_TRUE(final_state.has_value());
    ASSERT_GE(final_state.value(), 0);
    EXPECT_TRUE(runnable->is_accept(final_state.value()).value());

    // A codepoint without a transition answers -1.
    EXPECT_EQ(runnable->step(0, 'z').value(), NFARunAutomaton::MISSING);

    // Unknown states are rejected.
    EXPECT_FALSE(runnable->is_accept(42).has_value());
    EXPECT_FALSE(runnable->step(42, 'a').has_value());
}

TEST(NFARunAutomatonTest, SizeGrowsWithVisitedStates) {
    auto automaton = Automata::make_string("abcdef");
    ASSERT_TRUE(automaton.has_value());
    auto runnable = NFARunAutomaton::create(std::move(automaton.value()));
    ASSERT_TRUE(runnable.has_value());

    const size_t initial_size = runnable->get_size();
    EXPECT_GE(initial_size, 1u);

    EXPECT_TRUE(runnable->run_codepoints(code_points("abcdef")).value());
    EXPECT_GE(runnable->get_size(), initial_size);
}

TEST(NFARunAutomatonTest, TransitionsPartitionTheAlphabet) {
    auto automaton = Automata::make_string("foo");
    ASSERT_TRUE(automaton.has_value());
    auto runnable = NFARunAutomaton::create(std::move(automaton.value()));
    ASSERT_TRUE(runnable.has_value());

    Transition t;
    const int32_t num_transitions = runnable->init_transition(0, t).value();
    EXPECT_EQ(num_transitions, 1);

    ASSERT_TRUE(runnable->get_next_transition(t).has_value());
    EXPECT_EQ(t.min, 'f');
    EXPECT_EQ(t.max, 'f');
    EXPECT_GE(t.dest, 0);

    // get_transition returns the same transition by index.
    Transition by_index;
    ASSERT_TRUE(runnable->get_transition(0, 0, by_index).has_value());
    EXPECT_EQ(by_index.min, 'f');
    EXPECT_EQ(by_index.max, 'f');
    EXPECT_EQ(by_index.dest, t.dest);

    // Over-reading reports an error; there is no second transition.
    EXPECT_FALSE(runnable->get_next_transition(t).has_value());
    EXPECT_FALSE(runnable->get_transition(0, 1, by_index).has_value());
}

TEST(NFARunAutomatonTest, AnyStringHasOneFullRangeTransition) {
    auto automaton = Automata::make_any_string();
    ASSERT_TRUE(automaton.has_value());
    auto runnable = NFARunAutomaton::create(std::move(automaton.value()));
    ASSERT_TRUE(runnable.has_value());

    EXPECT_TRUE(runnable->run_codepoints(code_points("anything")).value());
    EXPECT_TRUE(runnable->run_codepoints(std::vector<int32_t>{}).value());

    Transition t;
    EXPECT_EQ(runnable->init_transition(0, t).value(), 1);
    ASSERT_TRUE(runnable->get_next_transition(t).has_value());
    EXPECT_EQ(t.min, 0);
    EXPECT_EQ(t.max, MAX_CODE_POINT);
    EXPECT_EQ(t.dest, 0);
}

TEST(NFARunAutomatonTest, ByteAlphabet) {
    const std::vector<BytesRef> terms{BytesRef::from_string("bar"), BytesRef::from_string("foo")};
    auto automaton = Automata::make_binary_string_union(terms);
    ASSERT_TRUE(automaton.has_value());

    auto runnable = NFARunAutomaton::create_with_alphabet_size(std::move(automaton.value()), 256);
    ASSERT_TRUE(runnable.has_value());

    EXPECT_TRUE(runnable->run_codepoints(std::vector<int32_t>{'b', 'a', 'r'}).value());
    EXPECT_TRUE(runnable->run_codepoints(std::vector<int32_t>{'f', 'o', 'o'}).value());
    EXPECT_FALSE(runnable->run_codepoints(std::vector<int32_t>{'b', 'a', 'z'}).value());

    // Codepoints outside the alphabet are rejected.
    EXPECT_FALSE(runnable->get_char_class(256).has_value());
    EXPECT_EQ(runnable->get_char_class(0).value(), 0u);
}

TEST(NFARunAutomatonTest, CharClassMatchesThePoints) {
    auto automaton = Automata::make_string("ab");
    ASSERT_TRUE(automaton.has_value());
    auto runnable = NFARunAutomaton::create(std::move(automaton.value()));
    ASSERT_TRUE(runnable.has_value());

    // Only 'a' and 'b' start an interval, everything between them shares a class.
    const size_t class_a = runnable->get_char_class('a').value();
    const size_t class_b = runnable->get_char_class('b').value();
    EXPECT_NE(class_a, class_b);
    EXPECT_EQ(runnable->get_char_class('a').value(), class_a);
    EXPECT_EQ(runnable->get_char_class(0).value(), 0u);
}
