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

export module lucene_core:reg_exp;

import :types;
import std;

import :automaton;
import :automaton_provider;
import :error;

namespace lucene {

/// The type of expression represented by a [`RegExp`] node.
export enum class RegExpKind {
    /// The union of two expressions.
    kUnion,
    /// A sequence of two expressions.
    kConcatenation,
    /// The intersection of two expressions.
    kIntersection,
    /// An optional expression.
    kOptional,
    /// An expression that repeats.
    kRepeat,
    /// An expression that repeats a minimum number of times.
    kRepeatMin,
    /// An expression that repeats a minimum and maximum number of times.
    kRepeatMinMax,
    /// The complement of a character class.
    kComplement,
    /// A character.
    kChar,
    /// A character range.
    kCharRange,
    /// Any character allowed.
    kAnyChar,
    /// An empty expression.
    kEmpty,
    /// A string expression.
    kString,
    /// Any string allowed.
    kAnyString,
    /// An automaton expression.
    kAutomaton,
    /// An interval expression.
    kInterval,
    /// An expression for a predefined class, e.g. `\w`.
    kPreClass,
    /// The complement of an expression (deprecated).
    kDeprecatedComplement,
};

/// Regular expression extension to [`Automaton`].
///
/// Regular expressions are built from the following abstract syntax:
///
/// ```text
/// regexp         ::= unionexp
///
/// unionexp       ::= interexp '|' unionexp          (union)
///                 |  interexp
///
/// interexp       ::= concatexp '&' interexp         (intersection) [OPTIONAL]
///                 |  concatexp
///
/// concatexp      ::= repeatexp concatexp            (concatenation)
///                 |  repeatexp
///
/// repeatexp      ::= repeatexp '?'                  (zero or one occurrence)
///                 |  repeatexp '*'                  (zero or more occurrences)
///                 |  repeatexp '+'                  (one or more occurrences)
///                 |  repeatexp '{n}'                (n occurrences)
///                 |  repeatexp '{n,}'               (n or more occurrences)
///                 |  repeatexp '{n,m}'              (n to m occurrences, inclusive)
///                 |  complexp
///
/// complexp       ::= charclassexp
///                 |  simpleexp
///
/// charclassexp   ::= '[' charclasses ']'            (character class)
///                 |  '[^' charclasses ']'           (negated character class)
///                 |  simpleexp
///
/// charclasses    ::= charclass charclasses
///                 |  charclass
///
/// charclass      ::= charexp '-' charexp            (character range, inclusive)
///                 |  charexp
///
/// simpleexp      ::= charexp
///                 |  '.'                            (any single character)
///                 |  '#'                            (empty language) [OPTIONAL]
///                 |  '@'                            (any string) [OPTIONAL]
///                 |  "\"" <Unicode string> "\""     (a string)
///                 |  "()"                           (the empty string)
///                 |  '(' unionexp ')'               (precedence override)
///                 |  '<' identifier '>'             (named automaton) [OPTIONAL]
///                 |  '<n-m>'                        (numerical interval) [OPTIONAL]
///
/// charexp        ::= <Unicode character>            (a single non-reserved character)
///                 |  \d                             (a digit [0-9])
///                 |  \D                             (a non-digit [^0-9])
///                 |  \s                             (whitespace [ \t\n\r])
///                 |  \S                             (non-whitespace)
///                 |  \w                             (a word character [a-zA-Z_0-9])
///                 |  \W                             (a non-word character [^\w])
///                 |  \\<Unicode character>          (an escaped character)
/// ```
///
/// Productions marked [OPTIONAL] are only allowed if specified by the syntax
/// flags used to create the [`RegExp`].
///
/// Reserved characters used in the enabled syntax must be escaped with backslash
/// (`\`) or double-quotes (`"..."`); this escaping is also required inside
/// character classes.  Be aware that dash (`-`) has a special meaning in
/// charclass expressions.
///
/// An identifier is a string not containing a right angle bracket (`>`) or a
/// dash (`-`).
///
/// Numerical intervals are specified by non-negative decimal integers and
/// include both end points; if `n` and `m` have the same number of digits then
/// the conforming strings must have that length (i.e. be prefixed by zeroes).
///
/// Port of `RegExp` (`core/util/automation/reg_exp.rs`).
///
/// **Deviation:** rlucene returns `Cow<Automaton>` from the conversion so that a
/// named automaton taken from the map is not copied; the C++ port always returns
/// an owned [`Automaton`] (the extra copy only happens for named automata).
export class RegExp {
public:
    /// Enables the intersection (`&`) operator.
    static constexpr int32_t INTERSECTION = 0x0001;
    /// Enables the empty language operator (`#`).
    static constexpr int32_t EMPTY = 0x0004;
    /// Enables the any string operator (`@`).
    static constexpr int32_t ANYSTRING = 0x0008;
    /// Enables named automata (`<identifier>`).
    static constexpr int32_t AUTOMATON = 0x0010;
    /// Enables numerical intervals (`<n-m>`).
    static constexpr int32_t INTERVAL = 0x0020;
    /// Enables all optional operators.
    static constexpr int32_t ALL = 0xff;
    /// Enables no optional operator.
    static constexpr int32_t NONE = 0x0000;
    /// Makes `\` and `"` escapes case insensitive for ASCII characters.
    static constexpr int32_t ASCII_CASE_INSENSITIVE = 0x0100;
    /// Enables the deprecated `~` complement operator.
    static constexpr int32_t DEPRECATED_COMPLEMENT = 0x10000;

    /// Parses `s` with `ALL` syntax flags; equivalent to
    /// `parse(s, ALL, 0)`.
    [[nodiscard]] static Result<RegExp> from_string(std::string_view s);
    /// Parses `s` with the given syntax flags.
    [[nodiscard]] static Result<RegExp> from_string_with_flags(std::string_view s, int32_t syntax_flags);
    /// Parses `s` into a regular expression tree.
    [[nodiscard]] static Result<RegExp> parse(std::string s, int32_t syntax_flags, int32_t match_flags);
    /// Builds a node out of its parts; the flags are the ones of the resulting
    /// node.
    [[nodiscard]] static RegExp create(int32_t flags, RegExpKind kind, std::unique_ptr<RegExp> exp1,
                                       std::unique_ptr<RegExp> exp2, std::string s, int32_t c, int32_t min,
                                       int32_t max, int32_t digits, int32_t from, int32_t to);

    RegExp(RegExp &&) noexcept = default;
    RegExp &operator=(RegExp &&) noexcept = default;
    RegExp(const RegExp &) = delete;
    RegExp &operator=(const RegExp &) = delete;
    /// Tears the tree down iteratively, so that deeply nested expressions cannot
    /// overflow the stack.
    ~RegExp();

    // ---- conversion to automata ----

    /// A new [`Automaton`] from this expression, with an empty automaton map.
    [[nodiscard]] Result<Automaton> to_automaton() const;
    /// A new [`Automaton`] from this expression, resolving named automata in
    /// `automata`; fails when a name is missing from both `automata` and the
    /// default provider.
    [[nodiscard]] Result<Automaton> to_automaton_from_map(const std::map<std::string, Automaton> &automata) const;
    /// A new [`Automaton`] from this expression, resolving named automata
    /// through `provider`.
    [[nodiscard]] Result<Automaton> to_automaton_from_provider(const AutomatonProvider &provider) const;

    // ---- inspection ----

    /// The string this expression was parsed from.
    [[nodiscard]] const std::string &get_original_string() const noexcept { return original_string; }
    /// Appends the canonical form of this expression to `b`.
    [[nodiscard]] VoidResult to_string_builder(std::string &b) const;
    /// The canonical form of this expression.
    [[nodiscard]] Result<std::string> to_string() const;
    /// Like [`to_string`], but shows the hierarchy more clearly.
    [[nodiscard]] Result<std::string> to_string_tree() const;
    /// The identifiers of all named automata occurring in this expression.
    [[nodiscard]] std::set<std::string> get_identifiers_set() const;

    // ---- parser state ----

    [[nodiscard]] Result<RegExp> parse_union_exp();
    [[nodiscard]] Result<RegExp> parse_inter_exp();
    [[nodiscard]] Result<RegExp> parse_concat_exp();
    [[nodiscard]] Result<RegExp> parse_repeat_exp();
    [[nodiscard]] Result<RegExp> parse_compl_exp();
    [[nodiscard]] Result<RegExp> parse_char_class_exp();
    [[nodiscard]] Result<RegExp> parse_char_classes();
    [[nodiscard]] Result<RegExp> parse_char_class();
    [[nodiscard]] Result<RegExp> parse_simple_exp();
    [[nodiscard]] Result<std::optional<RegExp>> match_predefined_character_class();
    [[nodiscard]] Result<int32_t> parse_char_exp();

    /// The type of expression.
    RegExpKind kind{RegExpKind::kEmpty};
    /// Child expressions held by a container expression.
    std::unique_ptr<RegExp> exp1{};
    std::unique_ptr<RegExp> exp2{};
    /// String expression.
    std::string s{};
    /// Character expression.
    int32_t c{0};
    /// Limits for repeatable expressions.
    int32_t min{0};
    int32_t max{0};
    int32_t digits{0};
    int32_t from{0};
    int32_t to{0};
    /// The whole string this expression was parsed from.
    std::string original_string{};
    /// The syntax and match flags.
    int32_t flags{0};
    /// The parse position, in bytes.
    size_t pos{0};

private:
    RegExp() = default;

    [[nodiscard]] Result<Automaton> to_automaton_impl(const std::map<std::string, Automaton> &automata,
                                                      const AutomatonProvider &provider) const;
    [[nodiscard]] VoidResult find_leaves(RegExpKind kind, std::vector<Automaton> &list,
                                         const std::map<std::string, Automaton> &automata,
                                         const AutomatonProvider &provider) const;
    [[nodiscard]] Result<Automaton> to_case_insensitive_string() const;
    static Result<Automaton> to_case_insensitive_char(int32_t codepoint);

    [[nodiscard]] Result<RegExp> expand_predefined() const;
    [[nodiscard]] VoidResult to_string_tree_from_string(std::string &b, std::string_view indent) const;
    void get_identifiers(std::set<std::string> &set) const;

    // ---- parser helpers ----

    /// Whether the next character is one of `s`.
    [[nodiscard]] bool peek(std::string_view s) const;
    /// Consumes `c` when it is next.
    bool match_char(int32_t c);
    /// Whether there is more input.
    [[nodiscard]] bool more() const noexcept { return pos < original_string.size(); }
    /// Consumes and returns the next codepoint.
    [[nodiscard]] Result<int32_t> next();
    /// Whether `flag` is set.
    [[nodiscard]] bool check(int32_t flag) const noexcept { return (flags & flag) != 0; }
};

} // namespace lucene
