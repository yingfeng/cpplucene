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

module lucene_core:ut.automata;

import lucene_core;

using namespace lucene;

namespace {

constexpr size_t WORK_LIMIT = Operations::DEFAULT_DETERMINIZE_WORK_LIMIT;

BytesRef bytes_of(std::string_view text) { return BytesRef::from_string(text); }

IntsRef ints_of(std::string_view text) {
    auto code_points = StringHelper::utf8_to_code_points(text);
    EXPECT_TRUE(code_points.has_value());
    return IntsRef(std::move(code_points.value()));
}

bool accepts(const Automaton &a, std::string_view text) { return Operations::run_str(a, text); }

/// The automaton a borrow-or-own result stands for: the owned value when there
/// is one, `fallback` (the borrowed input) otherwise.
const Automaton &resolved(const std::optional<Automaton> &value, const Automaton &fallback) {
    return value.has_value() ? value.value() : fallback;
}

/// An iterator over a fixed list of terms, for the `make_*_from_iter` entry
/// points.
class VectorBytesRefIterator final : public BytesRefIterator {
public:
    explicit VectorBytesRefIterator(const std::vector<BytesRef> &terms) : terms_(terms) {}

    [[nodiscard]] Result<const BytesRef *> next() override {
        if (index_ >= terms_.size()) {
            return static_cast<const BytesRef *>(nullptr);
        }
        return &terms_[index_++];
    }

private:
    const std::vector<BytesRef> &terms_;
    size_t index_{0};
};

} // namespace

// ---------------------------------------------------------------------------
// basic construction
// ---------------------------------------------------------------------------

TEST(AutomataTest, MakeStringAcceptsExactlyThatString) {
    auto automaton = Automata::make_string("foobar");
    ASSERT_TRUE(automaton.has_value());

    EXPECT_TRUE(automaton->is_deterministic());
    EXPECT_TRUE(accepts(automaton.value(), "foobar"));
    EXPECT_FALSE(accepts(automaton.value(), "fooba"));
    EXPECT_FALSE(accepts(automaton.value(), "foobarbaz"));
    EXPECT_FALSE(accepts(automaton.value(), ""));
}

TEST(AutomataTest, MakeStringHandlesNonAsciiCodepoints) {
    auto automaton = Automata::make_string("boo\xF0\x9F\x98\x82");
    ASSERT_TRUE(automaton.has_value());
    EXPECT_TRUE(accepts(automaton.value(), "boo\xF0\x9F\x98\x82"));
    EXPECT_FALSE(accepts(automaton.value(), "boo"));
}

TEST(AutomataTest, MakeEmptyAndEmptyString) {
    auto empty = Automata::make_empty();
    ASSERT_TRUE(empty.has_value());
    EXPECT_EQ(empty->get_num_states(), 0);
    EXPECT_TRUE(Operations::is_empty(empty.value()).value());
    EXPECT_FALSE(accepts(empty.value(), ""));

    auto empty_string = Automata::make_empty_string();
    ASSERT_TRUE(empty_string.has_value());
    EXPECT_EQ(empty_string->get_num_states(), 1);
    EXPECT_TRUE(accepts(empty_string.value(), ""));
    EXPECT_FALSE(accepts(empty_string.value(), "a"));
    EXPECT_FALSE(Operations::is_empty(empty_string.value()).value());
}

TEST(AutomataTest, MakeAnyStringAcceptsEverything) {
    auto any = Automata::make_any_string();
    ASSERT_TRUE(any.has_value());
    EXPECT_TRUE(accepts(any.value(), ""));
    EXPECT_TRUE(accepts(any.value(), "foobar"));
    EXPECT_TRUE(Operations::is_total(any.value()).value());
}

TEST(AutomataTest, MakeCharRange) {
    auto range = Automata::make_char_range('a', 'b');
    ASSERT_TRUE(range.has_value());
    EXPECT_TRUE(accepts(range.value(), "a"));
    EXPECT_TRUE(accepts(range.value(), "b"));
    EXPECT_FALSE(accepts(range.value(), "c"));
    EXPECT_FALSE(accepts(range.value(), ""));

    auto inverted = Automata::make_char_range('b', 'a');
    ASSERT_TRUE(inverted.has_value());
    EXPECT_EQ(inverted->get_num_states(), 0);
}

TEST(AutomataTest, AppendCharGrowsTheAutomaton) {
    // A fresh automaton cannot be extended after its last state is finished, so
    // append on a hand built one instead.
    Automaton a;
    (void)a.create_state();
    auto state = Automata::append_char(a, 0, 'x');
    ASSERT_TRUE(state.has_value());
    a.set_accept(state.value(), true);
    ASSERT_TRUE(a.finish_state().has_value());
    EXPECT_TRUE(accepts(a, "x"));
    EXPECT_FALSE(accepts(a, "y"));
}

TEST(AutomataTest, MakeDecimalInterval) {
    auto interval = Automata::make_decimal_interval(17, 100, 3);
    ASSERT_TRUE(interval.has_value());

    auto deterministic = Operations::determinize(interval.value(), WORK_LIMIT);
    ASSERT_TRUE(deterministic.has_value());
    const Automaton &a = deterministic->has_value() ? deterministic->value() : interval.value();

    EXPECT_FALSE(accepts(a, ""));
    EXPECT_TRUE(accepts(a, "017"));
    EXPECT_TRUE(accepts(a, "100"));
    EXPECT_TRUE(accepts(a, "073"));
    EXPECT_FALSE(accepts(a, "016"));
    EXPECT_FALSE(accepts(a, "101"));
    EXPECT_FALSE(accepts(a, "17"));
}

TEST(AutomataTest, MakeDecimalIntervalRejectsInvalidArguments) {
    EXPECT_FALSE(Automata::make_decimal_interval(100, 17, 3).has_value());
    // 1001 does not fit into three digits.
    EXPECT_FALSE(Automata::make_decimal_interval(17, 1001, 3).has_value());
    // 100 does fit, so the same arguments without the extra digit are fine.
    EXPECT_TRUE(Automata::make_decimal_interval(17, 100, 3).has_value());
}

TEST(AutomataTest, MakeBinaryInterval) {
    const BytesRef min = bytes_of("bar");
    const BytesRef max = bytes_of("foo");

    auto automaton = Automata::make_binary_interval(&min, true, &max, true);
    ASSERT_TRUE(automaton.has_value());
    EXPECT_TRUE(automaton->is_deterministic());

    const IntsRef bar = ints_of("bar");
    const IntsRef foo = ints_of("foo");
    const IntsRef beep = ints_of("beep");
    const IntsRef baq = ints_of("baq");
    const IntsRef bara = ints_of("bara");

    EXPECT_TRUE(Operations::run_ints_ref(automaton.value(), bar));
    EXPECT_TRUE(Operations::run_ints_ref(automaton.value(), foo));
    EXPECT_TRUE(Operations::run_ints_ref(automaton.value(), beep));
    EXPECT_FALSE(Operations::run_ints_ref(automaton.value(), baq));
    EXPECT_TRUE(Operations::run_ints_ref(automaton.value(), bara));
}

TEST(AutomataTest, MakeBinaryIntervalEqualBounds) {
    const BytesRef min = bytes_of("bar");

    auto automaton = Automata::make_binary_interval(&min, true, &min, true);
    ASSERT_TRUE(automaton.has_value());

    const IntsRef bar = ints_of("bar");
    const IntsRef bara = ints_of("bara");
    EXPECT_TRUE(Operations::run_ints_ref(automaton.value(), bar));
    EXPECT_FALSE(Operations::run_ints_ref(automaton.value(), bara));

    auto excluded = Automata::make_binary_interval(&min, false, &min, true);
    ASSERT_TRUE(excluded.has_value());
    EXPECT_EQ(excluded->get_num_states(), 0);
}

TEST(AutomataTest, MakeBinaryIntervalHandlesEmptyLowerBound) {
    const BytesRef empty;
    const BytesRef max = bytes_of("bar");

    auto automaton = Automata::make_binary_interval(&empty, true, &max, true);
    ASSERT_TRUE(automaton.has_value());

    const IntsRef nothing = ints_of("");
    const IntsRef a = ints_of("a");
    const IntsRef bar = ints_of("bar");
    const IntsRef bara = ints_of("bara");
    const IntsRef baz = ints_of("baz");

    EXPECT_TRUE(Operations::run_ints_ref(automaton.value(), nothing));
    EXPECT_TRUE(Operations::run_ints_ref(automaton.value(), a));
    EXPECT_TRUE(Operations::run_ints_ref(automaton.value(), bar));
    EXPECT_FALSE(Operations::run_ints_ref(automaton.value(), bara));
    EXPECT_FALSE(Operations::run_ints_ref(automaton.value(), baz));

    auto exclusive = Automata::make_binary_interval(&empty, false, &max, true);
    ASSERT_TRUE(exclusive.has_value());
    EXPECT_FALSE(Operations::run_ints_ref(exclusive.value(), nothing));
    EXPECT_TRUE(Operations::run_ints_ref(exclusive.value(), a));
    EXPECT_TRUE(Operations::run_ints_ref(exclusive.value(), bar));
}

TEST(AutomataTest, MakeBinaryIntervalRequiresInclusiveOpenBounds) {
    EXPECT_FALSE(Automata::make_binary_interval(nullptr, false, nullptr, true).has_value());
    EXPECT_FALSE(Automata::make_binary_interval(nullptr, true, nullptr, false).has_value());
}

TEST(AutomataTest, MakeBinaryIntervalWithoutBoundsIsAnyBinary) {
    auto any = Automata::make_binary_interval(nullptr, true, nullptr, true);
    ASSERT_TRUE(any.has_value());
    EXPECT_TRUE(Operations::is_total_with_range(any.value(), 0, 255).value());

    // Both bounds open at once is only expressible as "any term", so the
    // non-empty variant is reached through an explicit empty lower bound.
    EXPECT_FALSE(Automata::make_binary_interval(nullptr, false, nullptr, true).has_value());

    const BytesRef empty;
    auto lower_bound = Automata::make_binary_interval(&empty, false, nullptr, true);
    ASSERT_TRUE(lower_bound.has_value());
    EXPECT_TRUE(Operations::run_ints_ref(lower_bound.value(), IntsRef(std::vector<int32_t>{0})));
    EXPECT_FALSE(Operations::run_ints_ref(lower_bound.value(), IntsRef(std::vector<int32_t>{})));
}

// ---------------------------------------------------------------------------
// concatenation, union and closures
// ---------------------------------------------------------------------------

TEST(AutomataTest, Concatenate) {
    auto m = Automata::make_string("m");
    auto any = Automata::make_any_string();
    ASSERT_TRUE(m.has_value());
    ASSERT_TRUE(any.has_value());

    auto a = Operations::concatenate(m.value(), any.value());
    ASSERT_TRUE(a.has_value());
    EXPECT_TRUE(accepts(a.value(), "m"));
    EXPECT_TRUE(accepts(a.value(), "me"));
    EXPECT_FALSE(accepts(a.value(), ""));
    EXPECT_FALSE(accepts(a.value(), "e"));
}

TEST(AutomataTest, ConcatenateList) {
    auto foo = Automata::make_string("foo");
    auto bar = Automata::make_string("bar");
    ASSERT_TRUE(foo.has_value());
    ASSERT_TRUE(bar.has_value());

    auto a = Operations::concatenate_list({&foo.value(), &bar.value()});
    ASSERT_TRUE(a.has_value());
    EXPECT_TRUE(accepts(a.value(), "foobar"));
    EXPECT_FALSE(accepts(a.value(), "foo"));
    EXPECT_FALSE(accepts(a.value(), "bar"));
}

TEST(AutomataTest, Union) {
    auto a1 = Automata::make_string("foobar");
    auto a2 = Automata::make_string("barbaz");
    ASSERT_TRUE(a1.has_value());
    ASSERT_TRUE(a2.has_value());

    auto a = Operations::union_with(a1.value(), a2.value());
    ASSERT_TRUE(a.has_value());
    EXPECT_TRUE(accepts(a.value(), "foobar"));
    EXPECT_TRUE(accepts(a.value(), "barbaz"));
    EXPECT_FALSE(accepts(a.value(), "bar"));
    EXPECT_FALSE(Operations::has_dead_states(a.value()).value());
}

TEST(AutomataTest, UnionListWithEmptyString) {
    auto a1 = Automata::make_string("foobar");
    auto a2 = Automata::make_empty_string();
    auto a3 = Automata::make_string("barbaz");
    ASSERT_TRUE(a1.has_value());
    ASSERT_TRUE(a2.has_value());
    ASSERT_TRUE(a3.has_value());

    auto a = Operations::union_list({&a1.value(), &a2.value(), &a3.value()});
    ASSERT_TRUE(a.has_value());
    EXPECT_TRUE(accepts(a.value(), "foobar"));
    EXPECT_TRUE(accepts(a.value(), ""));
    EXPECT_TRUE(accepts(a.value(), "barbaz"));
}

TEST(AutomataTest, Optional) {
    auto a = Automata::make_string("foobar");
    ASSERT_TRUE(a.has_value());

    auto optional = Operations::optional(a.value());
    ASSERT_TRUE(optional.has_value());
    ASSERT_TRUE(optional->has_value());
    auto deterministic = Operations::determinize(optional->value(), WORK_LIMIT);
    ASSERT_TRUE(deterministic.has_value());
    const Automaton &result = deterministic->has_value() ? deterministic->value() : optional->value();

    EXPECT_TRUE(accepts(result, "foobar"));
    EXPECT_TRUE(accepts(result, ""));
    EXPECT_FALSE(accepts(result, "foo"));

    // Accepting the empty string already: the input is handed back.
    auto empty_string = Automata::make_empty_string();
    ASSERT_TRUE(empty_string.has_value());
    auto unchanged = Operations::optional(empty_string.value());
    ASSERT_TRUE(unchanged.has_value());
    EXPECT_FALSE(unchanged->has_value());
}

TEST(AutomataTest, Repeat) {
    auto a = Automata::make_string("zee");
    ASSERT_TRUE(a.has_value());

    auto repeated = Operations::repeat(a.value());
    ASSERT_TRUE(repeated.has_value());
    ASSERT_TRUE(repeated->has_value());
    auto deterministic = Operations::determinize(repeated->value(), WORK_LIMIT);
    ASSERT_TRUE(deterministic.has_value());

    EXPECT_TRUE(accepts(resolved(deterministic.value(), repeated->value()), ""));
    EXPECT_TRUE(accepts(resolved(deterministic.value(), repeated->value()), "zee"));
    EXPECT_TRUE(accepts(resolved(deterministic.value(), repeated->value()), "zeezee"));
    EXPECT_TRUE(accepts(resolved(deterministic.value(), repeated->value()), "zeezeezee"));
    EXPECT_FALSE(accepts(resolved(deterministic.value(), repeated->value()), "ze"));
}

TEST(AutomataTest, RepeatCount) {
    auto a = Automata::make_string("zee");
    ASSERT_TRUE(a.has_value());

    auto repeated = Operations::repeat_count(a.value(), 2);
    ASSERT_TRUE(repeated.has_value());
    ASSERT_TRUE(repeated->has_value());
    auto deterministic = Operations::determinize(repeated->value(), WORK_LIMIT);
    ASSERT_TRUE(deterministic.has_value());

    EXPECT_FALSE(accepts(resolved(deterministic.value(), repeated->value()), ""));
    EXPECT_FALSE(accepts(resolved(deterministic.value(), repeated->value()), "zee"));
    EXPECT_TRUE(accepts(resolved(deterministic.value(), repeated->value()), "zeezee"));
    EXPECT_TRUE(accepts(resolved(deterministic.value(), repeated->value()), "zeezeezee"));
}

TEST(AutomataTest, RepeatMinMax) {
    auto a = Automata::make_string("zee");
    ASSERT_TRUE(a.has_value());

    auto zero_to_two = Operations::repeat_min_max(a.value(), 0, 2);
    ASSERT_TRUE(zero_to_two.has_value());
    auto deterministic = Operations::determinize(zero_to_two.value(), WORK_LIMIT);
    ASSERT_TRUE(deterministic.has_value());
    EXPECT_TRUE(accepts(resolved(deterministic.value(), zero_to_two.value()), ""));
    EXPECT_TRUE(accepts(resolved(deterministic.value(), zero_to_two.value()), "zee"));
    EXPECT_TRUE(accepts(resolved(deterministic.value(), zero_to_two.value()), "zeezee"));
    EXPECT_FALSE(accepts(resolved(deterministic.value(), zero_to_two.value()), "zeezeezee"));

    auto two_to_four = Operations::repeat_min_max(a.value(), 2, 4);
    ASSERT_TRUE(two_to_four.has_value());
    auto deterministic2 = Operations::determinize(two_to_four.value(), WORK_LIMIT);
    ASSERT_TRUE(deterministic2.has_value());
    EXPECT_FALSE(accepts(resolved(deterministic2.value(), two_to_four.value()), ""));
    EXPECT_FALSE(accepts(resolved(deterministic2.value(), two_to_four.value()), "zee"));
    EXPECT_TRUE(accepts(resolved(deterministic2.value(), two_to_four.value()), "zeezee"));
    EXPECT_TRUE(accepts(resolved(deterministic2.value(), two_to_four.value()), "zeezeezee"));
    EXPECT_TRUE(accepts(resolved(deterministic2.value(), two_to_four.value()), "zeezeezeezee"));
    EXPECT_FALSE(accepts(resolved(deterministic2.value(), two_to_four.value()), "zeezeezeezeezee"));

    auto inverted = Operations::repeat_min_max(a.value(), 3, 2);
    ASSERT_TRUE(inverted.has_value());
    EXPECT_EQ(inverted->get_num_states(), 0);
}

// ---------------------------------------------------------------------------
// boolean algebra
// ---------------------------------------------------------------------------

TEST(AutomataTest, Complement) {
    auto a = Automata::make_string("zee");
    ASSERT_TRUE(a.has_value());

    auto complement = Operations::complement(a.value(), WORK_LIMIT);
    ASSERT_TRUE(complement.has_value());
    auto deterministic = Operations::determinize(complement.value(), WORK_LIMIT);
    ASSERT_TRUE(deterministic.has_value());

    EXPECT_TRUE(accepts(resolved(deterministic.value(), complement.value()), ""));
    EXPECT_FALSE(accepts(resolved(deterministic.value(), complement.value()), "zee"));
    EXPECT_TRUE(accepts(resolved(deterministic.value(), complement.value()), "zeezee"));
    EXPECT_TRUE(accepts(resolved(deterministic.value(), complement.value()), "ze"));
}

TEST(AutomataTest, Minus) {
    auto a1 = Automata::make_string("foobar");
    auto a2 = Automata::make_string("boobar");
    auto a3 = Automata::make_string("beebar");
    ASSERT_TRUE(a1.has_value());
    ASSERT_TRUE(a2.has_value());
    ASSERT_TRUE(a3.has_value());

    auto union_list = Operations::union_list({&a1.value(), &a2.value(), &a3.value()});
    ASSERT_TRUE(union_list.has_value());
    auto deterministic_union = Operations::determinize(union_list.value(), WORK_LIMIT);
    ASSERT_TRUE(deterministic_union.has_value());
    const Automaton &all = deterministic_union->has_value() ? deterministic_union->value() : union_list.value();

    auto without = Operations::minus(all, a2.value(), WORK_LIMIT);
    ASSERT_TRUE(without.has_value());
    ASSERT_TRUE(without->has_value());
    auto deterministic = Operations::determinize(without->value(), WORK_LIMIT);
    ASSERT_TRUE(deterministic.has_value());
    const Automaton &result = deterministic->has_value() ? deterministic->value() : without->value();

    EXPECT_TRUE(accepts(result, "foobar"));
    EXPECT_FALSE(accepts(result, "boobar"));
    EXPECT_TRUE(accepts(result, "beebar"));

    // Subtracting an empty language hands the input back.
    auto empty = Automata::make_empty();
    ASSERT_TRUE(empty.has_value());
    auto unchanged = Operations::minus(all, empty.value(), WORK_LIMIT);
    ASSERT_TRUE(unchanged.has_value());
    EXPECT_FALSE(unchanged->has_value());
}

TEST(AutomataTest, Intersection) {
    auto b_to_f = Automata::make_char_range('b', 'f');
    auto e_to_f = Automata::make_char_range('e', 'f');
    ASSERT_TRUE(b_to_f.has_value());
    ASSERT_TRUE(e_to_f.has_value());

    auto intersection = Operations::intersection(b_to_f.value(), e_to_f.value());
    ASSERT_TRUE(intersection.has_value());
    ASSERT_TRUE(intersection->has_value());

    EXPECT_TRUE(accepts(intersection->value(), "e"));
    EXPECT_TRUE(accepts(intersection->value(), "f"));
    EXPECT_FALSE(accepts(intersection->value(), "c"));
    EXPECT_FALSE(accepts(intersection->value(), ""));

    // An automaton without states intersects to the empty language.
    auto empty = Automata::make_empty();
    ASSERT_TRUE(empty.has_value());
    auto borrowed = Operations::intersection(b_to_f.value(), empty.value());
    ASSERT_TRUE(borrowed.has_value());
    ASSERT_TRUE(borrowed->has_value());
    EXPECT_EQ(borrowed->value().get_num_states(), 0);
}

// ---------------------------------------------------------------------------
// dead states and emptiness
// ---------------------------------------------------------------------------

TEST(AutomataTest, IsEmpty) {
    Automaton a;
    (void)a.create_state();
    EXPECT_TRUE(Operations::is_empty(a).value());

    auto empty = Automata::make_empty();
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(Operations::is_empty(empty.value()).value());

    auto any = Automata::make_any_string();
    ASSERT_TRUE(any.has_value());
    EXPECT_FALSE(Operations::is_empty(any.value()).value());
}

TEST(AutomataTest, RemoveDeadStates) {
    auto boo = Automata::make_string("boo");
    ASSERT_TRUE(boo.has_value());
    EXPECT_FALSE(Operations::has_dead_states(boo.value()).value());

    // 0 -a-> 1 (accept) -b-> 2: state 2 cannot reach an accept state.
    Automaton with_dead;
    (void)with_dead.create_state();
    (void)with_dead.create_state();
    (void)with_dead.create_state();
    ASSERT_TRUE(with_dead.add_transition_label(0, 1, 'a').has_value());
    with_dead.set_accept(1, true);
    ASSERT_TRUE(with_dead.add_transition_label(1, 2, 'b').has_value());
    ASSERT_TRUE(with_dead.finish_state().has_value());

    EXPECT_TRUE(Operations::has_dead_states(with_dead).value());
    EXPECT_TRUE(Operations::has_dead_states_from_initial(with_dead).value());
    EXPECT_FALSE(Operations::has_dead_states_to_accept(with_dead).value());

    auto reduced = Operations::remove_dead_states(with_dead);
    ASSERT_TRUE(reduced.has_value());
    ASSERT_TRUE(reduced->has_value());
    EXPECT_EQ(reduced->value().get_num_states(), 2);
    EXPECT_FALSE(Operations::has_dead_states(reduced->value()).value());
    EXPECT_TRUE(accepts(reduced->value(), "a"));
    EXPECT_FALSE(accepts(reduced->value(), "ab"));

    // Nothing to remove: the input is handed back.
    auto unchanged = Operations::remove_dead_states(boo.value());
    ASSERT_TRUE(unchanged.has_value());
    EXPECT_FALSE(unchanged->has_value());
}

TEST(AutomataTest, IsTotal) {
    EXPECT_FALSE(Operations::is_total(Automaton()).value());

    Automaton a;
    const int32_t init = a.create_state();
    const int32_t fini = a.create_state();
    a.set_accept(fini, true);
    ASSERT_TRUE(a.add_transition(init, fini, 0, MAX_CODE_POINT).has_value());
    ASSERT_TRUE(a.finish_state().has_value());
    EXPECT_FALSE(Operations::is_total(a).value());

    ASSERT_TRUE(a.add_transition(fini, fini, 0, MAX_CODE_POINT).has_value());
    ASSERT_TRUE(a.finish_state().has_value());
    EXPECT_FALSE(Operations::is_total(a).value());

    a.set_accept(init, true);
    EXPECT_TRUE(Operations::is_total(a).value());
}

TEST(AutomataTest, TotalizeAddsASink) {
    auto foo = Automata::make_string("foo");
    ASSERT_TRUE(foo.has_value());
    EXPECT_EQ(foo->step(0, 'z'), -1);

    auto total = Operations::totalize(foo.value());
    ASSERT_TRUE(total.has_value());

    // Every state now has a transition for every label; unknown strings end up
    // in the dead sink instead of failing early.
    EXPECT_GE(total->step(0, 'z'), 0);
    EXPECT_TRUE(accepts(total.value(), "foo"));
    EXPECT_FALSE(accepts(total.value(), "bar"));
    EXPECT_TRUE(Operations::has_dead_states(total.value()).value());
}

// ---------------------------------------------------------------------------
// common prefixes and suffixes
// ---------------------------------------------------------------------------

TEST(AutomataTest, CommonPrefix) {
    auto foobar = Automata::make_string("foobar");
    auto any = Automata::make_any_string();
    ASSERT_TRUE(foobar.has_value());
    ASSERT_TRUE(any.has_value());

    auto a = Operations::concatenate(foobar.value(), any.value());
    ASSERT_TRUE(a.has_value());
    auto prefix = Operations::get_common_prefix(a.value());
    ASSERT_TRUE(prefix.has_value());
    EXPECT_EQ(prefix.value(), "foobar");

    auto empty = Automata::make_empty();
    ASSERT_TRUE(empty.has_value());
    EXPECT_EQ(Operations::get_common_prefix(empty.value()).value(), "");

    auto empty_string = Automata::make_empty_string();
    ASSERT_TRUE(empty_string.has_value());
    EXPECT_EQ(Operations::get_common_prefix(empty_string.value()).value(), "");

    auto any_string = Automata::make_any_string();
    ASSERT_TRUE(any_string.has_value());
    EXPECT_EQ(Operations::get_common_prefix(any_string.value()).value(), "");
}

TEST(AutomataTest, CommonPrefixWithWildcards) {
    auto any_char = Automata::make_any_char();
    auto boo = Automata::make_string("boo");
    auto any_string = Automata::make_any_string();
    ASSERT_TRUE(any_char.has_value());
    ASSERT_TRUE(boo.has_value());
    ASSERT_TRUE(any_string.has_value());

    auto leading = Operations::concatenate(any_char.value(), boo.value());
    ASSERT_TRUE(leading.has_value());
    EXPECT_EQ(Operations::get_common_prefix(leading.value()).value(), "");

    auto trailing = Operations::concatenate(boo.value(), any_char.value());
    ASSERT_TRUE(trailing.has_value());
    EXPECT_EQ(Operations::get_common_prefix(trailing.value()).value(), "boo");

    auto leading_star = Operations::concatenate(any_string.value(), boo.value());
    ASSERT_TRUE(leading_star.has_value());
    EXPECT_EQ(Operations::get_common_prefix(leading_star.value()).value(), "");

    auto trailing_star = Operations::concatenate(boo.value(), any_string.value());
    ASSERT_TRUE(trailing_star.has_value());
    EXPECT_EQ(Operations::get_common_prefix(trailing_star.value()).value(), "boo");
}

TEST(AutomataTest, CommonPrefixRejectsDeadStates) {
    Automaton a;
    (void)a.create_state();
    (void)a.create_state();
    ASSERT_TRUE(a.add_transition_label(0, 1, 'a').has_value());
    ASSERT_TRUE(a.finish_state().has_value());

    auto prefix = Operations::get_common_prefix(a);
    ASSERT_FALSE(prefix.has_value());
    EXPECT_EQ(prefix.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(AutomataTest, CommonPrefixBytesRef) {
    auto automaton = Automata::make_string("boo");
    ASSERT_TRUE(automaton.has_value());

    auto prefix = Operations::get_common_prefix_bytes_ref(automaton.value());
    ASSERT_TRUE(prefix.has_value());
    EXPECT_EQ(prefix->as_slice().size(), 3u);
    EXPECT_EQ(prefix->utf8_to_string().value(), "boo");
}

TEST(AutomataTest, CommonSuffix) {
    auto automaton = Automata::make_string("boo");
    ASSERT_TRUE(automaton.has_value());
    auto suffix = Operations::get_common_suffix_bytes_ref(automaton.value());
    ASSERT_TRUE(suffix.has_value());
    EXPECT_EQ(suffix->utf8_to_string().value(), "boo");

    auto empty = Automata::make_empty();
    ASSERT_TRUE(empty.has_value());
    auto empty_suffix = Operations::get_common_suffix_bytes_ref(empty.value());
    ASSERT_TRUE(empty_suffix.has_value());
    EXPECT_EQ(empty_suffix->length(), 0u);

    auto empty_string = Automata::make_empty_string();
    ASSERT_TRUE(empty_string.has_value());
    auto empty_string_suffix = Operations::get_common_suffix_bytes_ref(empty_string.value());
    ASSERT_TRUE(empty_string_suffix.has_value());
    EXPECT_EQ(empty_string_suffix->length(), 0u);

    // A trailing wildcard destroys the common suffix.
    auto any_char = Automata::make_any_char();
    ASSERT_TRUE(any_char.has_value());
    auto trailing = Operations::concatenate(empty_string.value(), any_char.value());
    ASSERT_TRUE(trailing.has_value());
    auto wildcard_suffix = Operations::get_common_suffix_bytes_ref(trailing.value());
    ASSERT_TRUE(wildcard_suffix.has_value());
    EXPECT_EQ(wildcard_suffix->length(), 0u);
}

TEST(AutomataTest, CommonSuffixOfLeadingKleeneStar) {
    auto any = Automata::make_any_string();
    auto boo = Automata::make_string("boo");
    ASSERT_TRUE(any.has_value());
    ASSERT_TRUE(boo.has_value());

    auto a = Operations::concatenate(any.value(), boo.value());
    ASSERT_TRUE(a.has_value());
    auto suffix = Operations::get_common_suffix_bytes_ref(a.value());
    ASSERT_TRUE(suffix.has_value());
    EXPECT_EQ(suffix->utf8_to_string().value(), "boo");
}

// ---------------------------------------------------------------------------
// singleton and reversal
// ---------------------------------------------------------------------------

TEST(AutomataTest, GetSingleton) {
    auto automaton = Automata::make_string("foobar");
    ASSERT_TRUE(automaton.has_value());

    auto singleton = Operations::get_singleton(automaton.value());
    ASSERT_TRUE(singleton.has_value());
    ASSERT_TRUE(singleton->has_value());
    const IntsRef expected = ints_of("foobar");
    EXPECT_EQ(singleton->value().compare_to(expected), 0);
}

TEST(AutomataTest, GetSingletonEmptyString) {
    auto automaton = Automata::make_empty_string();
    ASSERT_TRUE(automaton.has_value());

    auto singleton = Operations::get_singleton(automaton.value());
    ASSERT_TRUE(singleton.has_value());
    ASSERT_TRUE(singleton->has_value());
    EXPECT_EQ(singleton->value().length(), 0u);
}

TEST(AutomataTest, GetSingletonOfNothingAndOfTwo) {
    Automaton nothing;
    (void)nothing.create_state();
    ASSERT_TRUE(nothing.finish_state().has_value());
    auto none = Operations::get_singleton(nothing);
    ASSERT_TRUE(none.has_value());
    EXPECT_FALSE(none->has_value());

    Automaton two;
    const int32_t s = two.create_state();
    const int32_t x = two.create_state();
    two.set_accept(x, true);
    ASSERT_TRUE(two.add_transition_label(s, x, 55).has_value());
    const int32_t y = two.create_state();
    two.set_accept(y, true);
    ASSERT_TRUE(two.add_transition_label(s, y, 58).has_value());
    ASSERT_TRUE(two.finish_state().has_value());

    auto both = Operations::get_singleton(two);
    ASSERT_TRUE(both.has_value());
    EXPECT_FALSE(both->has_value());
}

TEST(AutomataTest, ReverseRoundTrip) {
    auto original = Automata::make_string("foobar");
    ASSERT_TRUE(original.has_value());

    // Reversal introduces epsilon transitions; with a single accept state the
    // result stays deterministic, so determinizing may hand the input back.
    auto reversed = Operations::reverse(original.value());
    ASSERT_TRUE(reversed.has_value());
    auto deterministic = Operations::determinize(reversed.value(), WORK_LIMIT);
    ASSERT_TRUE(deterministic.has_value());
    EXPECT_TRUE(Operations::run_ints_ref(resolved(deterministic.value(), reversed.value()), ints_of("raboof")));

    auto twice = Operations::reverse(reversed.value());
    ASSERT_TRUE(twice.has_value());
    auto deterministic_twice = Operations::determinize(twice.value(), WORK_LIMIT);
    ASSERT_TRUE(deterministic_twice.has_value());
    EXPECT_TRUE(Operations::run_ints_ref(resolved(deterministic_twice.value(), twice.value()), ints_of("foobar")));
}

TEST(AutomataTest, ReverseEmptyLanguage) {
    auto empty = Automata::make_empty();
    ASSERT_TRUE(empty.has_value());

    auto reversed = Operations::reverse(empty.value());
    ASSERT_TRUE(reversed.has_value());
    EXPECT_EQ(reversed->get_num_states(), 0);
}

// ---------------------------------------------------------------------------
// ordering
// ---------------------------------------------------------------------------

TEST(AutomataTest, TopoSortStates) {
    auto automaton = Automata::make_string("abc");
    ASSERT_TRUE(automaton.has_value());

    auto sorted = Operations::topo_sort_states(automaton.value());
    ASSERT_TRUE(sorted.has_value());
    ASSERT_EQ(sorted->size(), 4u);
    EXPECT_EQ(sorted->front(), 0);
    EXPECT_EQ(sorted->back(), 3);

    // Every transition must go strictly forward in the ordering.
    std::vector<int32_t> position(4, -1);
    for (size_t i = 0; i < sorted->size(); ++i) {
        position[static_cast<size_t>((*sorted)[i])] = static_cast<int32_t>(i);
    }
    Transition t;
    for (const int32_t state : sorted.value()) {
        const int32_t count = automaton->init_transition(state, t);
        for (int32_t i = 0; i < count; ++i) {
            ASSERT_TRUE(automaton->get_next_transition(t).has_value());
            EXPECT_GT(position[static_cast<size_t>(t.dest)], position[static_cast<size_t>(state)]);
        }
    }
}

TEST(AutomataTest, TopoSortDetectsCycles) {
    auto any = Automata::make_any_string();
    ASSERT_TRUE(any.has_value());

    auto sorted = Operations::topo_sort_states(any.value());
    ASSERT_FALSE(sorted.has_value());
    EXPECT_EQ(sorted.error().kind(), ErrorKind::kIllegalArgument);
}

// ---------------------------------------------------------------------------
// string unions
// ---------------------------------------------------------------------------

TEST(AutomataTest, MakeStringUnion) {
    const std::vector<BytesRef> terms{bytes_of("bar"), bytes_of("foo"), bytes_of("foobar")};

    auto union_automaton = Automata::make_string_union(terms);
    ASSERT_TRUE(union_automaton.has_value());
    EXPECT_TRUE(union_automaton->is_deterministic());
    EXPECT_FALSE(Operations::has_dead_states_from_initial(union_automaton.value()).value());

    EXPECT_TRUE(accepts(union_automaton.value(), "bar"));
    EXPECT_TRUE(accepts(union_automaton.value(), "foo"));
    EXPECT_TRUE(accepts(union_automaton.value(), "foobar"));
    EXPECT_FALSE(accepts(union_automaton.value(), "ba"));
    EXPECT_FALSE(accepts(union_automaton.value(), "fo"));
    EXPECT_FALSE(accepts(union_automaton.value(), "foobarbaz"));

    // The automaton is minimal: no two states have the same transitions.
    EXPECT_EQ(union_automaton->get_num_states(), 7);
}

TEST(AutomataTest, MakeStringUnionOfNothingIsEmpty) {
    const std::vector<BytesRef> terms;
    auto union_automaton = Automata::make_string_union(terms);
    ASSERT_TRUE(union_automaton.has_value());
    EXPECT_EQ(union_automaton->get_num_states(), 0);
}

TEST(AutomataTest, MakeStringUnionRejectsUnsortedInput) {
    const std::vector<BytesRef> terms{bytes_of("foo"), bytes_of("bar")};
    auto union_automaton = Automata::make_string_union(terms);
    ASSERT_FALSE(union_automaton.has_value());
    EXPECT_EQ(union_automaton.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(AutomataTest, MakeStringUnionRejectsTooLongTerms) {
    std::string too_long(static_cast<size_t>(Automata::MAX_STRING_UNION_TERM_LENGTH) + 1, 'a');
    const std::vector<BytesRef> terms{bytes_of(too_long)};
    auto union_automaton = Automata::make_string_union(terms);
    ASSERT_FALSE(union_automaton.has_value());
    EXPECT_EQ(union_automaton.error().kind(), ErrorKind::kIllegalArgument);

    std::string at_the_limit(static_cast<size_t>(Automata::MAX_STRING_UNION_TERM_LENGTH), 'a');
    const std::vector<BytesRef> accepted_terms{bytes_of(at_the_limit)};
    EXPECT_TRUE(Automata::make_string_union(accepted_terms).has_value());
}

TEST(AutomataTest, MakeStringUnionUsesCodepointLabels) {
    const std::vector<BytesRef> terms{bytes_of("\xC3\xA9t\xC3\xA9")};
    auto union_automaton = Automata::make_string_union(terms);
    ASSERT_TRUE(union_automaton.has_value());

    // Codepoint labels (é, t, é): four states, not one per UTF-8 byte.
    EXPECT_EQ(union_automaton->get_num_states(), 4);
    EXPECT_TRUE(accepts(union_automaton.value(), "\xC3\xA9t\xC3\xA9"));
}

TEST(AutomataTest, MakeBinaryStringUnionUsesByteLabels) {
    const std::vector<BytesRef> terms{bytes_of("bar"), bytes_of("foo")};
    auto union_automaton = Automata::make_binary_string_union(terms);
    ASSERT_TRUE(union_automaton.has_value());

    EXPECT_TRUE(Operations::run_ints_ref(union_automaton.value(), IntsRef(std::vector<int32_t>{'b', 'a', 'r'})));
    EXPECT_TRUE(Operations::run_ints_ref(union_automaton.value(), IntsRef(std::vector<int32_t>{'f', 'o', 'o'})));
    EXPECT_FALSE(Operations::run_ints_ref(union_automaton.value(), IntsRef(std::vector<int32_t>{'b', 'a', 'z'})));
}

TEST(AutomataTest, MakeStringUnionFromIterator) {
    std::vector<BytesRef> terms{bytes_of("bar"), bytes_of("foo"), bytes_of("foobar")};
    VectorBytesRefIterator iterator(terms);

    auto union_automaton = Automata::make_string_union_from_iter(iterator);
    ASSERT_TRUE(union_automaton.has_value());
    EXPECT_TRUE(accepts(union_automaton.value(), "bar"));
    EXPECT_TRUE(accepts(union_automaton.value(), "foo"));
    EXPECT_TRUE(accepts(union_automaton.value(), "foobar"));
    EXPECT_FALSE(accepts(union_automaton.value(), "baz"));
}

TEST(AutomataTest, MakeBinaryStringUnionFromIterator) {
    std::vector<BytesRef> terms{bytes_of("bar"), bytes_of("foo")};
    VectorBytesRefIterator iterator(terms);

    auto union_automaton = Automata::make_binary_string_union_from_iter(iterator);
    ASSERT_TRUE(union_automaton.has_value());
    EXPECT_TRUE(Operations::run_ints_ref(union_automaton.value(), IntsRef(std::vector<int32_t>{'f', 'o', 'o'})));
    EXPECT_FALSE(Operations::run_ints_ref(union_automaton.value(), IntsRef(std::vector<int32_t>{'f', 'o'})));
}

TEST(AutomataTest, StringUnionIsMinimal) {
    // "abb" and "abc" share the accept state behind "ab" once minimized.
    const std::vector<BytesRef> terms{bytes_of("abb"), bytes_of("abc")};
    auto union_automaton = Automata::make_string_union(terms);
    ASSERT_TRUE(union_automaton.has_value());

    // 0 -a-> 1 -b-> 2, then 2 -b-> 3 and 2 -c-> 3.
    EXPECT_EQ(union_automaton->get_num_states(), 4);
    EXPECT_EQ(union_automaton->get_num_transitions(), 3);
}

TEST(AutomataTest, StringUnionOfDuplicates) {
    const std::vector<BytesRef> terms{bytes_of("foo"), bytes_of("foo")};
    auto union_automaton = Automata::make_string_union(terms);
    ASSERT_TRUE(union_automaton.has_value());
    EXPECT_EQ(union_automaton->get_num_states(), 4);
    EXPECT_TRUE(accepts(union_automaton.value(), "foo"));
}

// ---------------------------------------------------------------------------
// UTF-8 helpers used by the automata
// ---------------------------------------------------------------------------

TEST(AutomataTest, Utf8HelpersRoundTrip) {
    size_t num_bytes = 0;
    const auto code_point = StringHelper::utf8_code_point_at(std::string_view("\xF0\x9F\x98\x82"), 0, num_bytes);
    ASSERT_TRUE(code_point.has_value());
    EXPECT_EQ(code_point.value(), 0x1F602);
    EXPECT_EQ(num_bytes, 4u);

    std::string out;
    ASSERT_TRUE(StringHelper::utf8_append(out, 0x1F602).has_value());
    EXPECT_EQ(out, "\xF0\x9F\x98\x82");

    // Surrogates and out of range values are not Unicode scalar values.
    EXPECT_FALSE(StringHelper::utf8_append(out, 0xD800).has_value());
    EXPECT_FALSE(StringHelper::utf8_append(out, 0x110000).has_value());
    EXPECT_FALSE(StringHelper::utf8_code_point_at(std::string_view("\xC3"), 0, num_bytes).has_value());
}
