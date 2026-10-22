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

module lucene_core:ut.automaton_utf8;

import lucene_core;

using namespace lucene;

namespace {

/// An automaton accepting `[min, max]` followed by nothing else.
Automaton single_char(int32_t min, int32_t max) {
    Automaton automaton;
    (void)automaton.create_state();
    (void)automaton.create_state();
    EXPECT_TRUE(automaton.add_transition(0, 1, min, max).has_value());
    automaton.set_accept(1, true);
    EXPECT_TRUE(automaton.finish_state().has_value());
    return automaton;
}

/// An automaton accepting the two code points `first` then `second`.
Automaton two_chars(int32_t first, int32_t second) {
    Automaton automaton;
    (void)automaton.create_state();
    (void)automaton.create_state();
    (void)automaton.create_state();
    EXPECT_TRUE(automaton.add_transition_label(0, 1, first).has_value());
    EXPECT_TRUE(automaton.add_transition_label(1, 2, second).has_value());
    automaton.set_accept(2, true);
    EXPECT_TRUE(automaton.finish_state().has_value());
    return automaton;
}

bool run_bytes(ByteRunnable &runnable, const std::vector<uint8_t> &bytes) {
    auto accepted = runnable.run(std::span<const uint8_t>(bytes), 0, bytes.size());
    EXPECT_TRUE(accepted.has_value()) << accepted.error().to_string();
    return accepted.value();
}

} // namespace

TEST(OperationsTest, ReturnsNulloptForDeterministicInput) {
    Automaton automaton = single_char('a', 'c');
    auto determinized = Operations::determinize(automaton, 1000);
    ASSERT_TRUE(determinized.has_value()) << determinized.error().to_string();
    // Nothing to do: the caller keeps using the input.
    EXPECT_FALSE(determinized.value().has_value());

    // A single state automaton is trivially deterministic as well.
    AutomatonBuilder builder;
    (void)builder.create_state();
    builder.set_accept(0, true);
    auto finished = builder.finish();
    ASSERT_TRUE(finished.has_value());
    auto single = Operations::determinize(finished.value(), 1000);
    ASSERT_TRUE(single.has_value());
    EXPECT_FALSE(single.value().has_value());
}

TEST(OperationsTest, DeterminizesOverlappingTransitions) {
    // 0 -[a-c]-> 1, 0 -[b-d]-> 2, 1 -[e]-> 3, 2 -[e]-> 3, 3 accepting.
    AutomatonBuilder builder;
    (void)builder.create_state();
    (void)builder.create_state();
    (void)builder.create_state();
    (void)builder.create_state();
    builder.set_accept(3, true);
    ASSERT_TRUE(builder.add_transition(0, 1, 'a', 'c').has_value());
    ASSERT_TRUE(builder.add_transition(0, 2, 'b', 'd').has_value());
    ASSERT_TRUE(builder.add_transition_label(1, 3, 'e').has_value());
    ASSERT_TRUE(builder.add_transition_label(2, 3, 'e').has_value());
    auto nfa = builder.finish();
    ASSERT_TRUE(nfa.has_value()) << nfa.error().to_string();
    ASSERT_FALSE(nfa.value().is_deterministic());

    auto determinized = Operations::determinize(nfa.value(), 1000);
    ASSERT_TRUE(determinized.has_value()) << determinized.error().to_string();
    ASSERT_TRUE(determinized.value().has_value());
    const Automaton &dfa = determinized.value().value();
    EXPECT_TRUE(dfa.is_deterministic());
    // Subset construction: {0}, {1}, {1,2}, {2}, {3}.
    EXPECT_EQ(dfa.get_num_states(), 5);

    // "be" matches as well because both branches merge on 'e'.
    auto run = [&dfa](std::initializer_list<int32_t> labels) {
        int32_t state = 0;
        for (const int32_t label : labels) {
            state = dfa.step(state, label);
            if (state == -1) {
                return false;
            }
        }
        return dfa.is_accept(state);
    };
    EXPECT_FALSE(run({'a'}));
    EXPECT_FALSE(run({'d'}));
    EXPECT_TRUE(run({'a', 'e'}));
    EXPECT_TRUE(run({'c', 'e'}));
    EXPECT_TRUE(run({'d', 'e'}));
    // 'b' leads to states 1 and 2 at once, so 'b' + 'e' reaches the accept state.
    EXPECT_TRUE(run({'b', 'e'}));
    EXPECT_FALSE(run({'b'}));
}

TEST(OperationsTest, DeterminizeHonoursTheWorkLimit) {
    AutomatonBuilder builder;
    (void)builder.create_state();
    (void)builder.create_state();
    (void)builder.create_state();
    ASSERT_TRUE(builder.add_transition(0, 1, 'a', 'c').has_value());
    ASSERT_TRUE(builder.add_transition(0, 2, 'b', 'd').has_value());
    auto nfa = builder.finish();
    ASSERT_TRUE(nfa.has_value());

    auto determinized = Operations::determinize(nfa.value(), 0);
    ASSERT_FALSE(determinized.has_value());
    EXPECT_EQ(determinized.error().kind(), ErrorKind::kTooComplexToDeterminize);
}

TEST(Utf32ToUtf8Test, ConvertsSingleByteAndTwoByteCodePoints) {
    // A one byte code point stays one byte.
    auto ascii = ByteRunAutomaton::create(single_char('a', 'a'));
    ASSERT_TRUE(ascii.has_value()) << ascii.error().to_string();
    EXPECT_TRUE(run_bytes(ascii.value(), {'a'}));
    EXPECT_FALSE(run_bytes(ascii.value(), {'b'}));
    EXPECT_FALSE(run_bytes(ascii.value(), {0xC3, 0xA1}));

    // U+00E9 is C3 A9 in UTF-8, which spans two states of the converted
    // automaton.
    auto e_acute = ByteRunAutomaton::create(single_char(0xE9, 0xE9));
    ASSERT_TRUE(e_acute.has_value()) << e_acute.error().to_string();
    EXPECT_TRUE(run_bytes(e_acute.value(), {0xC3, 0xA9}));
    EXPECT_FALSE(run_bytes(e_acute.value(), {0xE9}));
    EXPECT_FALSE(run_bytes(e_acute.value(), {0xC3}));
    EXPECT_FALSE(run_bytes(e_acute.value(), {0xC3, 0xA8}));
}

TEST(Utf32ToUtf8Test, ConvertsThreeAndFourByteCodePoints) {
    // U+4E2D (the CJK character zhong) is E4 B8 AD.
    auto cjk = ByteRunAutomaton::create(single_char(0x4E2D, 0x4E2D));
    ASSERT_TRUE(cjk.has_value()) << cjk.error().to_string();
    EXPECT_TRUE(run_bytes(cjk.value(), {0xE4, 0xB8, 0xAD}));
    EXPECT_FALSE(run_bytes(cjk.value(), {0xE4, 0xB8}));
    EXPECT_FALSE(run_bytes(cjk.value(), {0xE4, 0xB9, 0xAD}));

    // U+1F600 (the grinning face emoji) is F0 9F 98 80.
    auto emoji = ByteRunAutomaton::create(single_char(0x1F600, 0x1F600));
    ASSERT_TRUE(emoji.has_value()) << emoji.error().to_string();
    EXPECT_TRUE(run_bytes(emoji.value(), {0xF0, 0x9F, 0x98, 0x80}));
    EXPECT_FALSE(run_bytes(emoji.value(), {0xF0, 0x9F, 0x98}));
}

TEST(Utf32ToUtf8Test, ConvertsMultiCodePointAutomaton) {
    auto automaton = two_chars('a', 0xE9);
    auto bytes = ByteRunAutomaton::create(automaton);
    ASSERT_TRUE(bytes.has_value()) << bytes.error().to_string();
    EXPECT_TRUE(run_bytes(bytes.value(), {'a', 0xC3, 0xA9}));
    EXPECT_FALSE(run_bytes(bytes.value(), {'a'}));
    EXPECT_FALSE(run_bytes(bytes.value(), {'a', 0xC3}));
    EXPECT_FALSE(run_bytes(bytes.value(), {0xC3, 0xA9, 'a'}));
}

TEST(Utf32ToUtf8Test, RejectsNonDeterministicAutomaton) {
    AutomatonBuilder builder;
    (void)builder.create_state();
    (void)builder.create_state();
    (void)builder.create_state();
    ASSERT_TRUE(builder.add_transition(0, 1, 'a', 'c').has_value());
    ASSERT_TRUE(builder.add_transition(0, 2, 'b', 'd').has_value());
    auto nfa = builder.finish();
    ASSERT_TRUE(nfa.has_value());

    auto bytes = ByteRunAutomaton::create(std::move(nfa.value()));
    ASSERT_FALSE(bytes.has_value());
    EXPECT_EQ(bytes.error().kind(), ErrorKind::kIllegalArgument);
}
