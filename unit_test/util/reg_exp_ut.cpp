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
#include <map>
#include <string>
#include <vector>

module lucene_core:ut.reg_exp;

import lucene_core;

using namespace lucene;

namespace {

constexpr size_t WORK_LIMIT = Operations::DEFAULT_DETERMINIZE_WORK_LIMIT;

/// Runs `text` against `a`, determinizing it first when necessary.
bool accepts(const Automaton &a, std::string_view text) {
    auto deterministic = Operations::determinize(a, WORK_LIMIT);
    const Automaton &det = deterministic.has_value() && deterministic->has_value() ? deterministic->value() : a;
    return Operations::run_str(det, text);
}

/// Parses `pattern` and converts it to an automaton.
Automaton compile(std::string_view pattern) {
    auto regexp = RegExp::from_string(pattern);
    EXPECT_TRUE(regexp.has_value()) << "pattern: " << pattern;
    if (!regexp.has_value()) {
        return Automaton();
    }
    auto automaton = regexp->to_automaton();
    EXPECT_TRUE(automaton.has_value()) << "pattern: " << pattern;
    if (!automaton.has_value()) {
        return Automaton();
    }
    return std::move(automaton.value());
}

/// The strings of a finite automaton, in iteration order.
std::vector<std::string> finite_strings(const Automaton &a) {
    FiniteStringsIterator iterator(a);
    std::vector<std::string> strings;
    while (true) {
        auto next = iterator.next();
        EXPECT_TRUE(next.has_value());
        if (!next.has_value() || !next->has_value()) {
            break;
        }
        std::string text;
        for (const int32_t label : next->value().as_slice()) {
            EXPECT_TRUE(StringHelper::utf8_append(text, label).has_value());
        }
        strings.push_back(std::move(text));
    }
    return strings;
}

} // namespace

// ---------------------------------------------------------------------------
// parsing and conversion
// ---------------------------------------------------------------------------

TEST(RegExpTest, SimpleConcatenation) {
    const Automaton a = compile("ab.*c");
    EXPECT_TRUE(accepts(a, "abc"));
    EXPECT_TRUE(accepts(a, "abzc"));
    EXPECT_TRUE(accepts(a, "abzzzc"));
    EXPECT_FALSE(accepts(a, "ab"));
    EXPECT_FALSE(accepts(a, "ac"));
    EXPECT_FALSE(accepts(a, "abcd"));

    auto regexp = RegExp::from_string("ab.*c");
    ASSERT_TRUE(regexp.has_value());
    auto text = regexp->to_string();
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text.value(), "\"ab\"(.)*\\c");
    EXPECT_EQ(regexp->get_original_string(), "ab.*c");
}

TEST(RegExpTest, Union) {
    const Automaton a = compile("(foo|bar)");
    EXPECT_TRUE(accepts(a, "foo"));
    EXPECT_TRUE(accepts(a, "bar"));
    EXPECT_FALSE(accepts(a, "foobar"));
    EXPECT_FALSE(accepts(a, ""));
}

TEST(RegExpTest, Intersection) {
    const Automaton a = compile("[a-z]&[a-c]");
    EXPECT_TRUE(accepts(a, "a"));
    EXPECT_TRUE(accepts(a, "c"));
    EXPECT_FALSE(accepts(a, "d"));
    EXPECT_FALSE(accepts(a, "ab"));

    // Without the INTERSECTION flag the `&` is a plain character.
    auto regexp = RegExp::from_string_with_flags("a&b", RegExp::ALL & ~RegExp::INTERSECTION);
    ASSERT_TRUE(regexp.has_value());
    auto automaton = regexp->to_automaton();
    ASSERT_TRUE(automaton.has_value());
    EXPECT_TRUE(accepts(automaton.value(), "a&b"));
}

TEST(RegExpTest, Repetition) {
    const Automaton any = compile("a*");
    EXPECT_TRUE(accepts(any, ""));
    EXPECT_TRUE(accepts(any, "aaaa"));

    const Automaton one_or_more = compile("a+");
    EXPECT_FALSE(accepts(one_or_more, ""));
    EXPECT_TRUE(accepts(one_or_more, "a"));
    EXPECT_TRUE(accepts(one_or_more, "aaaa"));

    const Automaton exactly_two = compile("a{2}");
    EXPECT_FALSE(accepts(exactly_two, "a"));
    EXPECT_TRUE(accepts(exactly_two, "aa"));
    EXPECT_FALSE(accepts(exactly_two, "aaa"));

    const Automaton two_to_three = compile("a{2,3}");
    EXPECT_FALSE(accepts(two_to_three, "a"));
    EXPECT_TRUE(accepts(two_to_three, "aa"));
    EXPECT_TRUE(accepts(two_to_three, "aaa"));
    EXPECT_FALSE(accepts(two_to_three, "aaaa"));

    const Automaton at_least_two = compile("a{2,}");
    EXPECT_FALSE(accepts(at_least_two, "a"));
    EXPECT_TRUE(accepts(at_least_two, "aa"));
    EXPECT_TRUE(accepts(at_least_two, "aaaa"));
}

TEST(RegExpTest, Optional) {
    const Automaton a = compile("ab?");
    EXPECT_TRUE(accepts(a, "a"));
    EXPECT_TRUE(accepts(a, "ab"));
    EXPECT_FALSE(accepts(a, "abb"));
}

TEST(RegExpTest, CharacterClasses) {
    const Automaton a = compile("[abc]");
    EXPECT_TRUE(accepts(a, "a"));
    EXPECT_TRUE(accepts(a, "c"));
    EXPECT_FALSE(accepts(a, "d"));

    const Automaton range = compile("[a-c]");
    EXPECT_TRUE(accepts(range, "b"));
    EXPECT_FALSE(accepts(range, "d"));

    const Automaton negated = compile("[^abc]");
    EXPECT_TRUE(accepts(negated, "d"));
    EXPECT_FALSE(accepts(negated, "a"));
    EXPECT_FALSE(accepts(negated, "ab"));
}

TEST(RegExpTest, PredefinedClasses) {
    const Automaton digit = compile("\\d");
    EXPECT_TRUE(accepts(digit, "5"));
    EXPECT_FALSE(accepts(digit, "a"));

    const Automaton non_digit = compile("\\D");
    EXPECT_TRUE(accepts(non_digit, "a"));
    EXPECT_FALSE(accepts(non_digit, "5"));

    const Automaton whitespace = compile("\\s");
    EXPECT_TRUE(accepts(whitespace, " "));
    EXPECT_TRUE(accepts(whitespace, "\t"));
    EXPECT_FALSE(accepts(whitespace, "a"));

    const Automaton word = compile("\\w");
    EXPECT_TRUE(accepts(word, "_"));
    EXPECT_TRUE(accepts(word, "z"));
    EXPECT_FALSE(accepts(word, "-"));

    const Automaton non_word = compile("\\W");
    EXPECT_TRUE(accepts(non_word, "-"));
    EXPECT_FALSE(accepts(non_word, "z"));
}

TEST(RegExpTest, StringAndEmptyString) {
    const Automaton a = compile("\"foo bar\"");
    EXPECT_TRUE(accepts(a, "foo bar"));
    EXPECT_FALSE(accepts(a, "foobar"));

    const Automaton empty = compile("()");
    EXPECT_TRUE(accepts(empty, ""));
    EXPECT_FALSE(accepts(empty, "a"));

    const Automaton any_string = compile("@");
    EXPECT_TRUE(accepts(any_string, ""));
    EXPECT_TRUE(accepts(any_string, "anything"));

    const Automaton empty_language = compile("#");
    EXPECT_FALSE(accepts(empty_language, ""));
    EXPECT_TRUE(Operations::is_empty(empty_language).value());
}

TEST(RegExpTest, Intervals) {
    const Automaton a = compile("<1-10>");
    EXPECT_TRUE(accepts(a, "1"));
    EXPECT_TRUE(accepts(a, "5"));
    EXPECT_TRUE(accepts(a, "10"));
    EXPECT_TRUE(accepts(a, "007"));
    EXPECT_FALSE(accepts(a, "0"));
    EXPECT_FALSE(accepts(a, "11"));

    // Equal digit counts require that many digits.
    const Automaton fixed = compile("<01-10>");
    EXPECT_TRUE(accepts(fixed, "01"));
    EXPECT_TRUE(accepts(fixed, "10"));
    EXPECT_FALSE(accepts(fixed, "1"));
    EXPECT_FALSE(accepts(fixed, "11"));
}

TEST(RegExpTest, IntervalRendering) {
    auto regexp = RegExp::from_string("<01-10>");
    ASSERT_TRUE(regexp.has_value());
    auto text = regexp->to_string();
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text.value(), "<01-10>");
}

TEST(RegExpTest, NamedAutomata) {
    auto regexp = RegExp::from_string("<foo>");
    ASSERT_TRUE(regexp.has_value());

    // Without a map the name cannot be resolved.
    auto missing = regexp->to_automaton();
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().kind(), ErrorKind::kIllegalArgument);

    std::map<std::string, Automaton> automata;
    auto foo = Automata::make_string("foo");
    ASSERT_TRUE(foo.has_value());
    automata.emplace("foo", std::move(foo.value()));

    auto resolved = regexp->to_automaton_from_map(automata);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_TRUE(accepts(resolved.value(), "foo"));
    EXPECT_FALSE(accepts(resolved.value(), "bar"));

    const AutomatonMapProvider provider(automata);
    auto provided = regexp->to_automaton_from_provider(provider);
    ASSERT_TRUE(provided.has_value());
    EXPECT_TRUE(accepts(provided.value(), "foo"));

    // The identifier set reports the name.
    const auto identifiers = regexp->get_identifiers_set();
    ASSERT_EQ(identifiers.size(), 1u);
    EXPECT_TRUE(identifiers.contains("foo"));

    // Without the AUTOMATON flag, `<foo>` is an interval syntax error.
    auto disabled = RegExp::from_string_with_flags("<foo>", RegExp::ALL & ~RegExp::AUTOMATON);
    ASSERT_FALSE(disabled.has_value());
    EXPECT_EQ(disabled.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(RegExpTest, CaseInsensitiveMatchFlag) {
    auto regexp = RegExp::parse("abc", RegExp::ALL, RegExp::ASCII_CASE_INSENSITIVE);
    ASSERT_TRUE(regexp.has_value());
    auto automaton = regexp->to_automaton();
    ASSERT_TRUE(automaton.has_value());
    EXPECT_TRUE(accepts(automaton.value(), "abc"));
    EXPECT_TRUE(accepts(automaton.value(), "ABC"));
    EXPECT_TRUE(accepts(automaton.value(), "AbC"));

    auto single = RegExp::parse("a", RegExp::ALL, RegExp::ASCII_CASE_INSENSITIVE);
    ASSERT_TRUE(single.has_value());
    auto single_automaton = single->to_automaton();
    ASSERT_TRUE(single_automaton.has_value());
    EXPECT_TRUE(accepts(single_automaton.value(), "A"));
    EXPECT_FALSE(accepts(single_automaton.value(), "b"));
}

TEST(RegExpTest, ToStringTree) {
    auto regexp = RegExp::from_string("a|b");
    ASSERT_TRUE(regexp.has_value());
    auto tree = regexp->to_string_tree();
    ASSERT_TRUE(tree.has_value());
    EXPECT_EQ(tree.value(), "Union\n  Char char=a\n  Char char=b\n");
}

TEST(RegExpTest, ParseErrors) {
    EXPECT_FALSE(RegExp::from_string("a{2,1}").has_value());
    EXPECT_FALSE(RegExp::from_string("[a").has_value());
    EXPECT_FALSE(RegExp::from_string("(").has_value());
    EXPECT_FALSE(RegExp::from_string("\\q").has_value());
    EXPECT_FALSE(RegExp::from_string("a)").has_value());
    EXPECT_FALSE(RegExp::from_string("<1-2-3>").has_value());
    EXPECT_FALSE(RegExp::from_string("[b-a]").has_value());

    // Illegal flag combinations.
    EXPECT_FALSE(RegExp::parse("a", 0x10000000, 0).has_value());
    EXPECT_FALSE(RegExp::parse("a", RegExp::ALL, RegExp::INTERSECTION).has_value());
}

TEST(RegExpTest, EmptyPatternIsTheEmptyString) {
    auto regexp = RegExp::from_string("");
    ASSERT_TRUE(regexp.has_value());
    auto automaton = regexp->to_automaton();
    ASSERT_TRUE(automaton.has_value());
    EXPECT_TRUE(accepts(automaton.value(), ""));
    EXPECT_FALSE(accepts(automaton.value(), "a"));
}

TEST(RegExpTest, NonAsciiPatterns) {
    const Automaton a = compile("\"café\"");
    EXPECT_TRUE(accepts(a, "café"));
    EXPECT_FALSE(accepts(a, "cafe"));
}

// ---------------------------------------------------------------------------
// finite strings iteration
// ---------------------------------------------------------------------------

TEST(FiniteStringsIteratorTest, SingleString) {
    auto automaton = Automata::make_string("abc");
    ASSERT_TRUE(automaton.has_value());
    EXPECT_EQ(finite_strings(automaton.value()), std::vector<std::string>{"abc"});
}

TEST(FiniteStringsIteratorTest, UnionOfStrings) {
    const std::vector<BytesRef> terms{BytesRef::from_string("bar"), BytesRef::from_string("foo")};
    auto automaton = Automata::make_string_union(terms);
    ASSERT_TRUE(automaton.has_value());
    EXPECT_EQ(finite_strings(automaton.value()), (std::vector<std::string>{"bar", "foo"}));
}

TEST(FiniteStringsIteratorTest, EmptyStringAndEmptyLanguage) {
    auto empty_string = Automata::make_empty_string();
    ASSERT_TRUE(empty_string.has_value());
    EXPECT_EQ(finite_strings(empty_string.value()), std::vector<std::string>{""});

    auto empty = Automata::make_empty();
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(finite_strings(empty.value()).empty());
}

TEST(FiniteStringsIteratorTest, CyclesAreReported) {
    auto automaton = Automata::make_any_string();
    ASSERT_TRUE(automaton.has_value());

    FiniteStringsIterator iterator(automaton.value());
    // The automaton accepts the empty string, then the self loop is a cycle.
    auto empty = iterator.next();
    ASSERT_TRUE(empty.has_value());
    ASSERT_TRUE(empty->has_value());
    EXPECT_EQ(empty->value().length(), 0u);

    auto next = iterator.next();
    EXPECT_FALSE(next.has_value());
    if (!next.has_value()) {
        EXPECT_EQ(next.error().kind(), ErrorKind::kIllegalArgument);
    }
}

TEST(LimitedFiniteStringsIteratorTest, RespectsTheLimit) {
    const std::vector<BytesRef> terms{BytesRef::from_string("bar"), BytesRef::from_string("baz"),
                                      BytesRef::from_string("foo")};
    auto automaton = Automata::make_string_union(terms);
    ASSERT_TRUE(automaton.has_value());

    auto limited = LimitedFiniteStringsIterator::create(automaton.value(), 2);
    ASSERT_TRUE(limited.has_value());

    std::vector<std::string> strings;
    while (true) {
        auto next = limited->next();
        ASSERT_TRUE(next.has_value());
        if (!next->has_value()) {
            break;
        }
        std::string text;
        for (const int32_t label : next->value().as_slice()) {
            ASSERT_TRUE(StringHelper::utf8_append(text, label).has_value());
        }
        strings.push_back(std::move(text));
    }
    EXPECT_EQ(strings, (std::vector<std::string>{"bar", "baz"}));
    EXPECT_EQ(limited->size(), 2);
}

TEST(LimitedFiniteStringsIteratorTest, UnlimitedAndInvalid) {
    auto automaton = Automata::make_string("abc");
    ASSERT_TRUE(automaton.has_value());

    auto unlimited = LimitedFiniteStringsIterator::create(automaton.value(), -1);
    ASSERT_TRUE(unlimited.has_value());
    auto first = unlimited->next();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(first->has_value());
    auto second = unlimited->next();
    ASSERT_TRUE(second.has_value());
    EXPECT_FALSE(second->has_value());
    EXPECT_EQ(unlimited->size(), 1);

    EXPECT_FALSE(LimitedFiniteStringsIterator::create(automaton.value(), 0).has_value());
    EXPECT_FALSE(LimitedFiniteStringsIterator::create(automaton.value(), -2).has_value());
}

// ---------------------------------------------------------------------------
// character run automaton
// ---------------------------------------------------------------------------

TEST(CharacterRunAutomatonTest, RunStr) {
    auto automaton = Automata::make_string("foo");
    ASSERT_TRUE(automaton.has_value());

    auto runnable = CharacterRunAutomaton::create(std::move(automaton.value()));
    ASSERT_TRUE(runnable.has_value());
    EXPECT_TRUE(runnable->run_str("foo").value());
    EXPECT_FALSE(runnable->run_str("bar").value());
    EXPECT_FALSE(runnable->run_str("fo").value());
}

TEST(CharacterRunAutomatonTest, RunChars) {
    auto automaton = Automata::make_string("ab");
    ASSERT_TRUE(automaton.has_value());
    auto runnable = CharacterRunAutomaton::create(std::move(automaton.value()));
    ASSERT_TRUE(runnable.has_value());

    const std::vector<uint16_t> chars{'a', 'b', 'c'};
    EXPECT_TRUE(runnable->run_chars(chars, 0, 2).value());
    EXPECT_FALSE(runnable->run_chars(chars, 1, 2).value());
    EXPECT_TRUE(runnable->run_chars(chars, 0, 0).value() == false);

    // An unpaired surrogate is rejected rather than decoded.
    const std::vector<uint16_t> broken{0xD800};
    EXPECT_FALSE(runnable->run_chars(broken, 0, 1).value());
}

TEST(CharacterRunAutomatonTest, SurrogatePairs) {
    // U+1F602, encoded in UTF-16 as the surrogate pair D83D DE02.
    auto automaton = Automata::make_string("\xF0\x9F\x98\x82");
    ASSERT_TRUE(automaton.has_value());
    auto runnable = CharacterRunAutomaton::create(std::move(automaton.value()));
    ASSERT_TRUE(runnable.has_value());

    const std::vector<uint16_t> chars{0xD83D, 0xDE02};
    EXPECT_TRUE(runnable->run_chars(chars, 0, 2).value());
    EXPECT_TRUE(runnable->run_str("\xF0\x9F\x98\x82").value());
    EXPECT_FALSE(runnable->run_str("a").value());
}

TEST(CharacterRunAutomatonTest, RejectsNonDeterministicAutomata) {
    auto any = Automata::make_any_char();
    auto any_char = Automata::make_char('a');
    ASSERT_TRUE(any.has_value());
    ASSERT_TRUE(any_char.has_value());

    auto united = Operations::union_with(any.value(), any_char.value());
    ASSERT_TRUE(united.has_value());
    auto runnable = CharacterRunAutomaton::create(std::move(united.value()));
    EXPECT_FALSE(runnable.has_value());
}
