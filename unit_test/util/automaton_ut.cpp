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

#include <array>
#include <cstdint>
#include <string>
#include <vector>

module lucene_core:ut.automaton;

import lucene_core;

using namespace lucene;

namespace {

/// Accepts exactly the strings "ab" and "c".
Automaton accept_ab_or_c() {
    Automaton automaton;
    const int32_t s0 = automaton.create_state();
    EXPECT_EQ(s0, 0);
    EXPECT_EQ(automaton.create_state(), 1);
    EXPECT_EQ(automaton.create_state(), 2);
    EXPECT_TRUE(automaton.add_transition_label(0, 1, 'a').has_value());
    EXPECT_TRUE(automaton.add_transition_label(0, 2, 'c').has_value());
    EXPECT_TRUE(automaton.add_transition_label(1, 2, 'b').has_value());
    automaton.set_accept(2, true);
    EXPECT_TRUE(automaton.finish_state().has_value());
    return automaton;
}

/// Adapter that runs a [`RunAutomaton`] over bytes; the byte level run
/// automaton itself is ported separately.
class ByteAdapter final : public ByteRunnable {
public:
    explicit ByteAdapter(RunAutomaton run_automaton) : run_automaton_(std::move(run_automaton)) {}

    [[nodiscard]] Result<int32_t> step(int32_t state, int32_t c) override { return run_automaton_.step(state, c); }
    [[nodiscard]] Result<bool> is_accept(int32_t state) const override { return run_automaton_.is_accept(state); }
    [[nodiscard]] size_t get_size() const override { return run_automaton_.size(); }

private:
    RunAutomaton run_automaton_;
};

} // namespace

TEST(AutomatonTest, StepsThroughLinearTransitions) {
    Automaton automaton = accept_ab_or_c();

    EXPECT_EQ(automaton.get_num_states(), 3);
    EXPECT_EQ(automaton.get_num_transitions(), 3);
    EXPECT_TRUE(automaton.is_deterministic());
    EXPECT_FALSE(automaton.is_accept(0));
    EXPECT_TRUE(automaton.is_accept(2));

    EXPECT_EQ(automaton.step(0, 'a'), 1);
    EXPECT_EQ(automaton.step(0, 'c'), 2);
    EXPECT_EQ(automaton.step(0, 'b'), -1);
    EXPECT_EQ(automaton.step(1, 'b'), 2);
    EXPECT_EQ(automaton.step(2, 'a'), -1);
    EXPECT_EQ(automaton.to_dot(), "0: 97-97->1 99-99->2\n1: 98-98->2\n2 [accept]:");
}

TEST(AutomatonTest, SortsAndMergesTransitions) {
    Automaton automaton;
    (void)automaton.create_state();
    (void)automaton.create_state();
    // Added out of order, and adjacent ranges to the same destination.
    ASSERT_TRUE(automaton.add_transition(0, 1, 6, 9).has_value());
    ASSERT_TRUE(automaton.add_transition(0, 1, 2, 5).has_value());
    ASSERT_TRUE(automaton.add_transition(0, 1, 1, 1).has_value());
    ASSERT_TRUE(automaton.add_transition(0, 1, 10, 10).has_value());
    automaton.set_accept(1, true);
    ASSERT_TRUE(automaton.finish_state().has_value());

    // 1-1, 2-9 and 10-10 all go to state 1, so they collapse into 1-10.
    EXPECT_EQ(automaton.get_num_transitions(), 1);
    Transition transition;
    EXPECT_EQ(automaton.init_transition(0, transition), 1);
    ASSERT_TRUE(automaton.get_next_transition(transition).has_value());
    EXPECT_EQ(transition.dest, 1);
    EXPECT_EQ(transition.min, 1);
    EXPECT_EQ(transition.max, 10);
}

TEST(AutomatonTest, DetectsNonDeterministicTransitions) {
    Automaton automaton;
    (void)automaton.create_state();
    (void)automaton.create_state();
    (void)automaton.create_state();
    ASSERT_TRUE(automaton.add_transition(0, 1, 1, 5).has_value());
    ASSERT_TRUE(automaton.add_transition(0, 2, 5, 9).has_value());
    ASSERT_TRUE(automaton.finish_state().has_value());
    EXPECT_FALSE(automaton.is_deterministic());
}

TEST(AutomatonTest, RejectsTransitionsAddedTwice) {
    Automaton automaton;
    (void)automaton.create_state();
    (void)automaton.create_state();
    ASSERT_TRUE(automaton.add_transition_label(0, 1, 'a').has_value());
    ASSERT_TRUE(automaton.add_transition_label(1, 0, 'b').has_value());
    // State 0 is already frozen, so adding to it again is an error.
    EXPECT_FALSE(automaton.add_transition_label(0, 1, 'c').has_value());
}

TEST(AutomatonTest, EpsilonCopiesTransitionsAndAcceptance) {
    Automaton automaton;
    (void)automaton.create_state();
    (void)automaton.create_state();
    (void)automaton.create_state();
    ASSERT_TRUE(automaton.add_transition_label(1, 2, 'x').has_value());
    automaton.set_accept(2, true);
    // A virtual epsilon from 0 to 1 copies its transitions onto state 0.
    ASSERT_TRUE(automaton.add_epsilon(0, 1).has_value());
    ASSERT_TRUE(automaton.finish_state().has_value());
    EXPECT_EQ(automaton.step(0, 'x'), 2);
    EXPECT_FALSE(automaton.is_accept(0));

    Automaton accepting;
    (void)accepting.create_state();
    (void)accepting.create_state();
    accepting.set_accept(1, true);
    ASSERT_TRUE(accepting.add_epsilon(0, 1).has_value());
    EXPECT_TRUE(accepting.is_accept(0));
}

TEST(AutomatonTest, StartPointsCoverEveryInterval) {
    Automaton automaton;
    (void)automaton.create_state();
    (void)automaton.create_state();
    ASSERT_TRUE(automaton.add_transition(0, 1, 'a', 'c').has_value());
    ASSERT_TRUE(automaton.add_transition(0, 1, 'x', 'x').has_value());
    ASSERT_TRUE(automaton.finish_state().has_value());

    const std::vector<int32_t> expected{0, 'a', 'd', 'x', 'y'};
    EXPECT_EQ(automaton.get_start_points(), expected);
}

TEST(AutomatonTest, BuilderCompilesUnsortedTransitions) {
    AutomatonBuilder builder;
    (void)builder.create_state();
    (void)builder.create_state();
    (void)builder.create_state();
    builder.set_accept(2, true);
    // Sources are interleaved: `finish` has to sort them.
    ASSERT_TRUE(builder.add_transition_label(1, 2, 'b').has_value());
    ASSERT_TRUE(builder.add_transition_label(0, 1, 'a').has_value());

    auto automaton = builder.finish();
    ASSERT_TRUE(automaton.has_value()) << automaton.error().to_string();
    EXPECT_EQ(automaton.value().step(0, 'a'), 1);
    EXPECT_EQ(automaton.value().step(1, 'b'), 2);
    EXPECT_TRUE(automaton.value().is_accept(2));
    EXPECT_TRUE(automaton.value().is_deterministic());
}

TEST(AutomatonTest, CopyRenumbersStates) {
    Automaton automaton = accept_ab_or_c();

    Automaton duplicated;
    ASSERT_TRUE(duplicated.copy(automaton).has_value());
    ASSERT_TRUE(duplicated.copy(automaton).has_value());
    ASSERT_TRUE(duplicated.finish_state().has_value());
    // The second copy starts at state 3.
    EXPECT_EQ(duplicated.get_num_states(), 6);
    EXPECT_EQ(duplicated.step(3, 'a'), 4);
    EXPECT_TRUE(duplicated.is_accept(5));

    // The builder copies a whole automaton into a new one.
    AutomatonBuilder builder;
    ASSERT_TRUE(builder.copy(automaton).has_value());
    auto built = builder.finish();
    ASSERT_TRUE(built.has_value()) << built.error().to_string();
    EXPECT_EQ(built.value().get_num_states(), 3);
    EXPECT_EQ(built.value().step(0, 'a'), 1);
    EXPECT_TRUE(built.value().is_accept(2));
}

TEST(RunAutomatonTest, RunsBytesOverAClassTable) {
    Automaton automaton = accept_ab_or_c();
    auto run_automaton = RunAutomaton::create(std::move(automaton), 256);
    ASSERT_TRUE(run_automaton.has_value()) << run_automaton.error().to_string();
    EXPECT_EQ(run_automaton.value().size(), 3);
    EXPECT_TRUE(run_automaton.value().is_accept(2).value());
    EXPECT_FALSE(run_automaton.value().is_accept(0).value());

    ByteAdapter adapter(run_automaton.value());
    const std::string ab = "ab";
    const std::string c = "c";
    const std::string abc = "abc";
    const std::string ac = "ac";
    EXPECT_TRUE(adapter.run(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(ab.data()), ab.size()), 0,
                            ab.size())
                        .value());
    EXPECT_TRUE(adapter.run(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(c.data()), c.size()), 0,
                            c.size())
                        .value());
    EXPECT_FALSE(adapter.run(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(abc.data()), abc.size()), 0,
                             abc.size())
                         .value());
    EXPECT_FALSE(adapter.run(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(ac.data()), ac.size()), 0,
                             ac.size())
                         .value());

    // "b" leaves the initial state.
    const std::string b = "b";
    EXPECT_FALSE(adapter.run(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(b.data()), b.size()), 0,
                             b.size())
                         .value());
}

TEST(RunAutomatonTest, RejectsNonDeterministicAutomaton) {
    Automaton automaton;
    (void)automaton.create_state();
    (void)automaton.create_state();
    (void)automaton.create_state();
    ASSERT_TRUE(automaton.add_transition(0, 1, 1, 5).has_value());
    ASSERT_TRUE(automaton.add_transition(0, 2, 5, 9).has_value());
    ASSERT_TRUE(automaton.finish_state().has_value());

    auto run_automaton = RunAutomaton::create(std::move(automaton), 256);
    ASSERT_FALSE(run_automaton.has_value());
    EXPECT_EQ(run_automaton.error().kind(), ErrorKind::kIllegalArgument);
}
