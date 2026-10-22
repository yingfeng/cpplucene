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
#include <string>
#include <vector>

module lucene_core:ut.levenshtein_automata;

import lucene_core;

using namespace lucene;

namespace {

std::vector<int32_t> code_points(std::string_view text) {
    auto decoded = StringHelper::utf8_to_code_points(text);
    EXPECT_TRUE(decoded.has_value());
    return decoded.has_value() ? std::move(decoded.value()) : std::vector<int32_t>{};
}

/// Plain Levenshtein distance.
int32_t edit_distance(const std::vector<int32_t> &left, const std::vector<int32_t> &right) {
    std::vector<int32_t> previous(right.size() + 1);
    std::vector<int32_t> current(right.size() + 1);
    for (size_t j = 0; j <= right.size(); ++j) {
        previous[j] = static_cast<int32_t>(j);
    }
    for (size_t i = 1; i <= left.size(); ++i) {
        current[0] = static_cast<int32_t>(i);
        for (size_t j = 1; j <= right.size(); ++j) {
            const int32_t cost = left[i - 1] == right[j - 1] ? 0 : 1;
            current[j] = std::min({previous[j] + 1, current[j - 1] + 1, previous[j - 1] + cost});
        }
        std::swap(previous, current);
    }
    return previous[right.size()];
}

/// Damerau-Levenshtein distance with adjacent transpositions (optimal string
/// alignment), which is what the `*t` descriptions model.
int32_t transposition_distance(const std::vector<int32_t> &left, const std::vector<int32_t> &right) {
    std::vector<std::vector<int32_t>> d(left.size() + 1, std::vector<int32_t>(right.size() + 1, 0));
    for (size_t i = 0; i <= left.size(); ++i) {
        d[i][0] = static_cast<int32_t>(i);
    }
    for (size_t j = 0; j <= right.size(); ++j) {
        d[0][j] = static_cast<int32_t>(j);
    }
    for (size_t i = 1; i <= left.size(); ++i) {
        for (size_t j = 1; j <= right.size(); ++j) {
            const int32_t cost = left[i - 1] == right[j - 1] ? 0 : 1;
            d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost});
            if (i > 1 && j > 1 && left[i - 1] == right[j - 2] && left[i - 2] == right[j - 1]) {
                d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + 1);
            }
        }
    }
    return d[left.size()][right.size()];
}

/// Every sequence of length `0..max_length` over `symbols`.
std::vector<std::vector<int32_t>> candidates(const std::vector<int32_t> &symbols, size_t max_length) {
    std::vector<std::vector<int32_t>> result;
    std::vector<int32_t> current;
    for (size_t length = 0; length <= max_length; ++length) {
        current.assign(length, 0);
        if (length == 0) {
            result.emplace_back();
            continue;
        }
        std::vector<size_t> index(length, 0);
        while (true) {
            for (size_t i = 0; i < length; ++i) {
                current[i] = symbols[index[i]];
            }
            result.push_back(current);

            size_t position = 0;
            while (position < length) {
                index[position] += 1;
                if (index[position] < symbols.size()) {
                    break;
                }
                index[position] = 0;
                position += 1;
            }
            if (position == length) {
                break;
            }
        }
    }
    return result;
}

bool accepts(const Automaton &a, const std::vector<int32_t> &text) {
    return Operations::run_ints_ref(a, IntsRef(text));
}

} // namespace

TEST(LevenshteinAutomataTest, ExactWordAtDistanceZero) {
    auto builder = LevenshteinAutomata::create("foobar", false);
    ASSERT_TRUE(builder.has_value());

    auto automaton = builder->to_automaton(0);
    ASSERT_TRUE(automaton.has_value());
    ASSERT_TRUE(automaton->has_value());
    EXPECT_TRUE(accepts(automaton->value(), code_points("foobar")));
    EXPECT_FALSE(accepts(automaton->value(), code_points("fooba")));
    EXPECT_FALSE(accepts(automaton->value(), code_points("foobars")));
}

TEST(LevenshteinAutomataTest, DistanceOneAndTwo) {
    auto builder = LevenshteinAutomata::create("foobar", false);
    ASSERT_TRUE(builder.has_value());

    auto one = builder->to_automaton(1);
    ASSERT_TRUE(one.has_value());
    ASSERT_TRUE(one->has_value());
    EXPECT_TRUE(accepts(one->value(), code_points("foobar")));
    EXPECT_TRUE(accepts(one->value(), code_points("fooba")));   // deletion
    EXPECT_TRUE(accepts(one->value(), code_points("foobars"))); // insertion
    EXPECT_TRUE(accepts(one->value(), code_points("faobar")));  // substitution
    EXPECT_FALSE(accepts(one->value(), code_points("fobar")) == false); // sanity, still one edit
    EXPECT_FALSE(accepts(one->value(), code_points("bar")));
    EXPECT_FALSE(accepts(one->value(), code_points("foobazbaz")));

    auto two = builder->to_automaton(2);
    ASSERT_TRUE(two.has_value());
    ASSERT_TRUE(two->has_value());
    EXPECT_TRUE(accepts(two->value(), code_points("fobar")));
    EXPECT_FALSE(accepts(two->value(), code_points("bar")));
    EXPECT_FALSE(accepts(two->value(), code_points("foobarbaz")));
    EXPECT_TRUE(accepts(two->value(), code_points("foabaz")));

    // Beyond the supported distance there is no automaton.
    auto three = builder->to_automaton(3);
    ASSERT_TRUE(three.has_value());
    EXPECT_FALSE(three->has_value());
    EXPECT_EQ(LevenshteinAutomata::MAXIMUM_SUPPORTED_DISTANCE, 2);
}

TEST(LevenshteinAutomataTest, MatchesBruteForceDistance) {
    const std::vector<int32_t> word{'a', 'b'};
    const std::vector<int32_t> symbols{'a', 'b', 'c', 'd'};
    const auto all = candidates(symbols, 4);

    for (const bool with_transpositions : {false, true}) {
        auto builder = LevenshteinAutomata::from_word(word, 'd', with_transpositions);
        ASSERT_TRUE(builder.has_value());

        for (const size_t n : {size_t{1}, size_t{2}}) {
            auto automaton = builder->to_automaton(n);
            ASSERT_TRUE(automaton.has_value());
            ASSERT_TRUE(automaton->has_value()) << "n=" << n;
            ASSERT_TRUE(automaton->value().is_deterministic());
            EXPECT_FALSE(Operations::has_dead_states(automaton->value()).value());

            for (const auto &text : all) {
                const int32_t distance =
                        with_transpositions ? transposition_distance(word, text) : edit_distance(word, text);
                const bool expected = distance <= static_cast<int32_t>(n);
                std::string printable;
                for (const int32_t symbol : text) {
                    printable.push_back(static_cast<char>(symbol));
                }
                EXPECT_EQ(accepts(automaton->value(), text), expected)
                        << "text='" << printable << "' n=" << n << " transpositions=" << with_transpositions;
            }
        }
    }
}

TEST(LevenshteinAutomataTest, MatchesBruteForceDistanceForLongerWord) {
    const std::vector<int32_t> word{'a', 'b', 'c'};
    const std::vector<int32_t> symbols{'a', 'b', 'c', 'd'};
    const auto all = candidates(symbols, 3);

    auto builder = LevenshteinAutomata::from_word(word, 'd', false);
    ASSERT_TRUE(builder.has_value());
    auto automaton = builder->to_automaton(2);
    ASSERT_TRUE(automaton.has_value());
    ASSERT_TRUE(automaton->has_value());

    for (const auto &text : all) {
        EXPECT_EQ(accepts(automaton->value(), text), edit_distance(word, text) <= 2);
    }
}

TEST(LevenshteinAutomataTest, TranspositionsCountAsOneEdit) {
    const std::vector<int32_t> word{'a', 'b'};

    auto plain = LevenshteinAutomata::from_word(word, 'z', false);
    ASSERT_TRUE(plain.has_value());
    auto plain_automaton = plain->to_automaton(1);
    ASSERT_TRUE(plain_automaton.has_value());
    ASSERT_TRUE(plain_automaton->has_value());
    EXPECT_FALSE(accepts(plain_automaton->value(), code_points("ba")));

    auto with_transpositions = LevenshteinAutomata::from_word(word, 'z', true);
    ASSERT_TRUE(with_transpositions.has_value());
    auto transposition_automaton = with_transpositions->to_automaton(1);
    ASSERT_TRUE(transposition_automaton.has_value());
    ASSERT_TRUE(transposition_automaton->has_value());
    EXPECT_TRUE(accepts(transposition_automaton->value(), code_points("ba")));
    // One deletion ("cab" -> "ab") still costs a single edit, so only strings
    // two edits away are rejected.
    EXPECT_TRUE(accepts(transposition_automaton->value(), code_points("cab")));
    EXPECT_FALSE(accepts(transposition_automaton->value(), code_points("cax")));
}

TEST(LevenshteinAutomataTest, Prefix) {
    auto builder = LevenshteinAutomata::create("bar", false);
    ASSERT_TRUE(builder.has_value());

    auto automaton = builder->to_automaton_with_prefix(1, "pre");
    ASSERT_TRUE(automaton.has_value());
    ASSERT_TRUE(automaton->has_value());

    EXPECT_TRUE(accepts(automaton->value(), code_points("prebar")));
    EXPECT_TRUE(accepts(automaton->value(), code_points("preba")));
    EXPECT_FALSE(accepts(automaton->value(), code_points("bar")));
    EXPECT_FALSE(accepts(automaton->value(), code_points("prebazbaz")));
}

TEST(LevenshteinAutomataTest, EmptyWord) {
    auto builder = LevenshteinAutomata::from_word(std::vector<int32_t>{}, 'z', false);
    ASSERT_TRUE(builder.has_value());

    auto one = builder->to_automaton(1);
    ASSERT_TRUE(one.has_value());
    ASSERT_TRUE(one->has_value());
    EXPECT_TRUE(accepts(one->value(), code_points("")));
    EXPECT_TRUE(accepts(one->value(), code_points("z")));
    EXPECT_FALSE(accepts(one->value(), code_points("zz")));

    auto two = builder->to_automaton(2);
    ASSERT_TRUE(two.has_value());
    ASSERT_TRUE(two->has_value());
    EXPECT_TRUE(accepts(two->value(), code_points("zz")));
    EXPECT_FALSE(accepts(two->value(), code_points("zzz")));
}

TEST(LevenshteinAutomataTest, RejectsSymbolsAboveAlphaMax) {
    auto builder = LevenshteinAutomata::from_word(std::vector<int32_t>{'a', 'z'}, 'y', false);
    ASSERT_FALSE(builder.has_value());
    EXPECT_EQ(builder.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(LevenshteinAutomataTest, NonAsciiWord) {
    // "café" has four codepoints, one of them above 127.
    auto builder = LevenshteinAutomata::create("café", false);
    ASSERT_TRUE(builder.has_value());
    ASSERT_EQ(builder->word().size(), 4u);

    auto automaton = builder->to_automaton(1);
    ASSERT_TRUE(automaton.has_value());
    ASSERT_TRUE(automaton->has_value());
    EXPECT_TRUE(accepts(automaton->value(), code_points("café")));
    EXPECT_TRUE(accepts(automaton->value(), code_points("cafe")));
    // Deleting the single non-ASCII codepoint is one edit.
    EXPECT_TRUE(accepts(automaton->value(), code_points("caf")));
    EXPECT_FALSE(accepts(automaton->value(), code_points("ca")));
}

TEST(LevenshteinAutomataTest, AlphabetAndRanges) {
    auto builder = LevenshteinAutomata::from_word(std::vector<int32_t>{'b', 'a', 'b'}, 'd', false);
    ASSERT_TRUE(builder.has_value());

    // The alphabet is sorted and deduplicated.
    EXPECT_EQ(builder->alphabet(), (std::vector<int32_t>{'a', 'b'}));
    EXPECT_EQ(builder->alpha_max(), static_cast<int32_t>('d'));
    // The ranges outside of the alphabet cover [0, 'a'), ('b', 'd'].
    EXPECT_EQ(builder->num_ranges(), 2u);
}
