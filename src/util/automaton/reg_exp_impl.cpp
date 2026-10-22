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

module lucene_core:reg_exp.impl;

import :reg_exp;

import :automata;
import :automaton;
import :automaton_provider;
import :debug;
import :error;
import :operations;
import :string_helper;
import :types;

import std;

namespace lucene {

namespace {

/// `Debug` name of a kind, as used by `to_string_tree`.
std::string_view kind_name(RegExpKind kind) {
    switch (kind) {
    case RegExpKind::kUnion:
        return "Union";
    case RegExpKind::kConcatenation:
        return "Concatenation";
    case RegExpKind::kIntersection:
        return "Intersection";
    case RegExpKind::kOptional:
        return "Optional";
    case RegExpKind::kRepeat:
        return "Repeat";
    case RegExpKind::kRepeatMin:
        return "RepeatMin";
    case RegExpKind::kRepeatMinMax:
        return "RepeatMinMax";
    case RegExpKind::kComplement:
        return "Complement";
    case RegExpKind::kChar:
        return "Char";
    case RegExpKind::kCharRange:
        return "CharRange";
    case RegExpKind::kAnyChar:
        return "AnyChar";
    case RegExpKind::kEmpty:
        return "Empty";
    case RegExpKind::kString:
        return "String";
    case RegExpKind::kAnyString:
        return "AnyString";
    case RegExpKind::kAutomaton:
        return "Automaton";
    case RegExpKind::kInterval:
        return "Interval";
    case RegExpKind::kPreClass:
        return "PreClass";
    case RegExpKind::kDeprecatedComplement:
        return "DeprecatedComplement";
    }
    return "?";
}

/// Appends `code_point` to `out` when it is a Unicode scalar value.
void append_code_point(std::string &out, int32_t code_point) {
    (void)StringHelper::utf8_append(out, code_point);
}

/// The UTF-8 text of a codepoint, empty when it is not a scalar value.
std::string code_point_text(int32_t code_point) {
    std::string text;
    if (!StringHelper::utf8_append(text, code_point).has_value()) {
        return std::string();
    }
    return text;
}

/// Appends the rendering of `t`, or `'?'` when it is not a scalar value.
std::string code_point_text_or_unknown(int32_t code_point) {
    std::string text = code_point_text(code_point);
    return text.empty() ? std::string("?") : text;
}

/// Appends the rendering of `child` to `b`, failing when it is missing.
VoidResult append_child(std::string &b, const RegExp *child) {
    if (child == nullptr) {
        return err(LuceneError::illegal_state("child expression is missing"));
    }
    return child->to_string_builder(b);
}

/// A node without children.
RegExp make_container_node(int32_t flags, RegExpKind kind, std::unique_ptr<RegExp> exp1 = nullptr,
                           std::unique_ptr<RegExp> exp2 = nullptr) {
    return RegExp::create(flags, kind, std::move(exp1), std::move(exp2), "", 0, 0, 0, 0, 0, 0);
}

/// A node with one child.
RegExp make_unary_node(int32_t flags, RegExpKind kind, RegExp exp) {
    return RegExp::create(flags, kind, std::make_unique<RegExp>(std::move(exp)), nullptr, "", 0, 0, 0, 0, 0, 0);
}

/// A node with two children.
RegExp make_binary_node(int32_t flags, RegExpKind kind, RegExp exp1, RegExp exp2) {
    return RegExp::create(flags, kind, std::make_unique<RegExp>(std::move(exp1)),
                          std::make_unique<RegExp>(std::move(exp2)), "", 0, 0, 0, 0, 0, 0);
}

RegExp make_leaf_node(int32_t flags, RegExpKind kind, std::string s, int32_t c, int32_t min, int32_t max,
                      int32_t digits, int32_t from, int32_t to) {
    return RegExp::create(flags, kind, nullptr, nullptr, std::move(s), c, min, max, digits, from, to);
}

RegExp make_char_node(int32_t flags, int32_t c) {
    return make_leaf_node(flags, RegExpKind::kChar, "", c, 0, 0, 0, 0, 0);
}

RegExp make_string_node(int32_t flags, std::string s) {
    return make_leaf_node(flags, RegExpKind::kString, std::move(s), 0, 0, 0, 0, 0, 0);
}

RegExp make_any_char_node(int32_t flags) { return make_container_node(flags, RegExpKind::kAnyChar); }

RegExp make_any_string_node(int32_t flags) { return make_container_node(flags, RegExpKind::kAnyString); }

RegExp make_empty_node(int32_t flags) { return make_container_node(flags, RegExpKind::kEmpty); }

Result<RegExp> make_char_range_node(int32_t flags, int32_t from, int32_t to) {
    if (from > to) {
        return err(LuceneError::illegal_argument(
                std::format("invalid range: from ({}) cannot be > to ({})", from, to)));
    }
    return make_leaf_node(flags, RegExpKind::kCharRange, "", 0, 0, 0, 0, from, to);
}

RegExp make_string_concat(int32_t flags, const RegExp &exp1, const RegExp &exp2) {
    std::string b;
    if (exp1.kind == RegExpKind::kString) {
        b += exp1.s;
    } else if (exp1.kind == RegExpKind::kChar) {
        append_code_point(b, exp1.c);
    }
    if (exp2.kind == RegExpKind::kString) {
        b += exp2.s;
    } else if (exp2.kind == RegExpKind::kChar) {
        append_code_point(b, exp2.c);
    }
    return make_string_node(flags, std::move(b));
}

bool is_string_or_char(const RegExp &exp) {
    return exp.kind == RegExpKind::kString || exp.kind == RegExpKind::kChar;
}

Result<RegExp> make_concatenation(int32_t flags, RegExp exp1, RegExp exp2) {
    if (is_string_or_char(exp1) && is_string_or_char(exp2)) {
        return make_string_concat(flags, exp1, exp2);
    }

    const bool merge_left = exp1.kind == RegExpKind::kConcatenation && exp1.exp2 != nullptr &&
                            is_string_or_char(*exp1.exp2) && is_string_or_char(exp2);
    if (merge_left) {
        std::unique_ptr<RegExp> rexp1 = std::move(exp1.exp1);
        std::unique_ptr<RegExp> e2 = std::move(exp1.exp2);
        if (rexp1 == nullptr || e2 == nullptr) {
            return err(LuceneError::illegal_state("concatenation expression is missing"));
        }
        RegExp rexp2 = make_string_concat(flags, *e2, exp2);
        return RegExp::create(flags, RegExpKind::kConcatenation, std::move(rexp1),
                              std::make_unique<RegExp>(std::move(rexp2)), "", 0, 0, 0, 0, 0, 0);
    }

    const bool merge_right = exp2.kind == RegExpKind::kConcatenation && exp2.exp1 != nullptr &&
                             is_string_or_char(*exp2.exp1) && is_string_or_char(exp1);
    if (merge_right) {
        std::unique_ptr<RegExp> e1 = std::move(exp2.exp1);
        std::unique_ptr<RegExp> rexp2 = std::move(exp2.exp2);
        if (e1 == nullptr || rexp2 == nullptr) {
            return err(LuceneError::illegal_state("concatenation expression is missing"));
        }
        RegExp rexp1 = make_string_concat(flags, exp1, *e1);
        return RegExp::create(flags, RegExpKind::kConcatenation, std::make_unique<RegExp>(std::move(rexp1)),
                              std::move(rexp2), "", 0, 0, 0, 0, 0, 0);
    }

    return make_binary_node(flags, RegExpKind::kConcatenation, std::move(exp1), std::move(exp2));
}

/// Parses a non-negative decimal number.
bool parse_non_negative_int(std::string_view text, int32_t &out) {
    const auto *begin = text.data();
    const auto *end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, out);
    return result.ec == std::errc{} && result.ptr == end;
}

} // namespace

RegExp::~RegExp() {
    // Tear the tree down iteratively so that deeply nested expressions cannot
    // overflow the stack when they are destroyed.
    std::vector<std::unique_ptr<RegExp>> stack;
    if (exp1 != nullptr) {
        stack.push_back(std::move(exp1));
    }
    if (exp2 != nullptr) {
        stack.push_back(std::move(exp2));
    }
    while (!stack.empty()) {
        std::unique_ptr<RegExp> node = std::move(stack.back());
        stack.pop_back();
        if (node->exp1 != nullptr) {
            stack.push_back(std::move(node->exp1));
        }
        if (node->exp2 != nullptr) {
            stack.push_back(std::move(node->exp2));
        }
        // `node` is released here, with both children already detached.
    }
}

RegExp RegExp::create(int32_t flags, RegExpKind kind, std::unique_ptr<RegExp> exp1, std::unique_ptr<RegExp> exp2,
                      std::string s, int32_t c, int32_t min, int32_t max, int32_t digits, int32_t from, int32_t to) {
    RegExp exp;
    exp.kind = kind;
    exp.exp1 = std::move(exp1);
    exp.exp2 = std::move(exp2);
    exp.s = std::move(s);
    exp.c = c;
    exp.min = min;
    exp.max = max;
    exp.digits = digits;
    exp.from = from;
    exp.to = to;
    exp.flags = flags;
    return exp;
}

// ---------------------------------------------------------------------------
// parsing
// ---------------------------------------------------------------------------

Result<RegExp> RegExp::from_string(std::string_view s) { return from_string_with_flags(s, ALL); }

Result<RegExp> RegExp::from_string_with_flags(std::string_view s, int32_t syntax_flags) {
    return parse(std::string(s), syntax_flags, 0);
}

Result<RegExp> RegExp::parse(std::string s, int32_t syntax_flags, int32_t match_flags) {
    if ((syntax_flags & ~DEPRECATED_COMPLEMENT) > ALL) {
        return err(LuceneError::illegal_argument("Illegal syntax flag"));
    }
    if (match_flags > 0 && match_flags <= ALL) {
        return err(LuceneError::illegal_argument("Illegal match flag"));
    }

    const int32_t combined_flags = syntax_flags | match_flags;

    RegExp parser;
    parser.kind = RegExpKind::kEmpty;
    parser.original_string = std::move(s);
    parser.flags = combined_flags;
    parser.pos = 0;

    RegExp e;
    if (parser.original_string.empty()) {
        e = make_string_node(combined_flags, "");
    } else {
        auto parsed = parser.parse_union_exp();
        if (!parsed) {
            return err(std::move(parsed.error()));
        }
        if (parser.pos < parser.original_string.size()) {
            return err(LuceneError::illegal_argument(
                    std::format("end-of-string expected at position {}", parser.pos)));
        }
        e = std::move(parsed.value());
    }
    e.original_string = std::move(parser.original_string);
    e.flags = combined_flags;
    e.pos = parser.pos;

    return e;
}

bool RegExp::peek(std::string_view chars) const {
    if (!more()) {
        return false;
    }
    size_t num_bytes = 0;
    auto code_point = StringHelper::utf8_code_point_at(original_string, pos, num_bytes);
    if (!code_point) {
        return false;
    }
    const std::string encoded = code_point_text(code_point.value());
    return chars.find(encoded) != std::string_view::npos;
}

bool RegExp::match_char(int32_t c) {
    if (!more()) {
        return false;
    }
    size_t num_bytes = 0;
    auto code_point = StringHelper::utf8_code_point_at(original_string, pos, num_bytes);
    if (!code_point || code_point.value() != c) {
        return false;
    }
    pos += num_bytes;
    return true;
}

Result<int32_t> RegExp::next() {
    if (!more()) {
        return err(LuceneError::illegal_argument("unexpected end-of-string"));
    }
    size_t num_bytes = 0;
    auto code_point = StringHelper::utf8_code_point_at(original_string, pos, num_bytes);
    if (!code_point) {
        return err(LuceneError::illegal_state("parser position is not on a character"));
    }
    pos += num_bytes;
    return code_point.value();
}

Result<RegExp> RegExp::parse_union_exp() {
    const int32_t exp_flags = flags;
    auto first = parse_inter_exp();
    if (!first) {
        return err(std::move(first.error()));
    }
    RegExp result = std::move(first.value());
    while (match_char('|')) {
        auto e = parse_inter_exp();
        if (!e) {
            return err(std::move(e.error()));
        }
        result = make_binary_node(exp_flags, RegExpKind::kUnion, std::move(result), std::move(e.value()));
    }
    return result;
}

Result<RegExp> RegExp::parse_inter_exp() {
    const int32_t exp_flags = flags;
    auto first = parse_concat_exp();
    if (!first) {
        return err(std::move(first.error()));
    }
    RegExp result = std::move(first.value());
    while (check(INTERSECTION) && match_char('&')) {
        auto e = parse_concat_exp();
        if (!e) {
            return err(std::move(e.error()));
        }
        result = make_binary_node(exp_flags, RegExpKind::kIntersection, std::move(result), std::move(e.value()));
    }
    return result;
}

Result<RegExp> RegExp::parse_concat_exp() {
    const int32_t exp_flags = flags;
    auto first = parse_repeat_exp();
    if (!first) {
        return err(std::move(first.error()));
    }
    RegExp result = std::move(first.value());
    while (more() && !peek(")|") && (!check(INTERSECTION) || !peek("&"))) {
        auto e = parse_repeat_exp();
        if (!e) {
            return err(std::move(e.error()));
        }
        auto concatenated = make_concatenation(exp_flags, std::move(result), std::move(e.value()));
        if (!concatenated) {
            return err(std::move(concatenated.error()));
        }
        result = std::move(concatenated.value());
    }
    return result;
}

Result<RegExp> RegExp::parse_repeat_exp() {
    auto first = parse_compl_exp();
    if (!first) {
        return err(std::move(first.error()));
    }
    RegExp e = std::move(first.value());

    while (peek("?*+{")) {
        if (match_char('?')) {
            e = make_unary_node(flags, RegExpKind::kOptional, std::move(e));
        } else if (match_char('*')) {
            e = make_unary_node(flags, RegExpKind::kRepeat, std::move(e));
        } else if (match_char('+')) {
            e = make_unary_node(flags, RegExpKind::kRepeatMin, std::move(e));
            e.min = 1;
        } else if (match_char('{')) {
            const size_t start = pos;
            while (peek("0123456789")) {
                auto consumed = next();
                if (!consumed) {
                    return err(std::move(consumed.error()));
                }
            }
            if (start == pos) {
                return err(LuceneError::illegal_argument(std::format("integer expected at position {}", pos)));
            }
            int32_t n = 0;
            if (!parse_non_negative_int(std::string_view(original_string).substr(start, pos - start), n)) {
                return err(LuceneError::illegal_argument(std::format("invalid number at position {}", pos)));
            }

            int32_t m = -1;
            if (match_char(',')) {
                const size_t max_start = pos;
                while (peek("0123456789")) {
                    auto consumed = next();
                    if (!consumed) {
                        return err(std::move(consumed.error()));
                    }
                }
                if (max_start != pos) {
                    if (!parse_non_negative_int(std::string_view(original_string).substr(max_start, pos - max_start),
                                                m)) {
                        return err(LuceneError::illegal_argument(std::format("invalid number at position {}", pos)));
                    }
                }
            } else {
                m = n;
            }

            if (!match_char('}')) {
                return err(LuceneError::illegal_argument(std::format("expected '}}' at position {}", pos)));
            }

            if (m != -1 && n > m) {
                return err(LuceneError::illegal_argument(
                        std::format("invalid repetition range (out of order): {}..{}", n, m)));
            }

            if (m == -1) {
                RegExp node = make_unary_node(flags, RegExpKind::kRepeatMin, std::move(e));
                node.min = n;
                e = std::move(node);
            } else {
                RegExp node = make_unary_node(flags, RegExpKind::kRepeatMinMax, std::move(e));
                node.min = n;
                node.max = m;
                e = std::move(node);
            }
        }
    }

    return e;
}

Result<RegExp> RegExp::parse_compl_exp() {
    if (check(DEPRECATED_COMPLEMENT) && match_char('~')) {
        auto sub = parse_compl_exp();
        if (!sub) {
            return err(std::move(sub.error()));
        }
        return make_unary_node(flags, RegExpKind::kDeprecatedComplement, std::move(sub.value()));
    }
    return parse_char_class_exp();
}

Result<RegExp> RegExp::parse_char_class_exp() {
    if (match_char('[')) {
        bool negate = false;
        if (match_char('^')) {
            negate = true;
        }
        auto classes = parse_char_classes();
        if (!classes) {
            return err(std::move(classes.error()));
        }
        RegExp e = std::move(classes.value());

        if (negate) {
            RegExp any = make_any_char_node(flags);
            RegExp not_e = make_unary_node(flags, RegExpKind::kComplement, std::move(e));
            e = make_binary_node(flags, RegExpKind::kIntersection, std::move(any), std::move(not_e));
        }
        if (!match_char(']')) {
            return err(LuceneError::illegal_argument(std::format("expected ']' at position {}", pos)));
        }
        return e;
    }
    return parse_simple_exp();
}

Result<RegExp> RegExp::parse_char_classes() {
    auto first = parse_char_class();
    if (!first) {
        return err(std::move(first.error()));
    }
    RegExp e = std::move(first.value());
    while (more() && !peek("]")) {
        auto next_class = parse_char_class();
        if (!next_class) {
            return err(std::move(next_class.error()));
        }
        e = make_binary_node(flags, RegExpKind::kUnion, std::move(e), std::move(next_class.value()));
    }
    return e;
}

Result<RegExp> RegExp::parse_char_class() {
    auto predefined = match_predefined_character_class();
    if (!predefined) {
        return err(std::move(predefined.error()));
    }
    if (predefined->has_value()) {
        return std::move(predefined->value());
    }

    auto c1 = parse_char_exp();
    if (!c1) {
        return err(std::move(c1.error()));
    }
    if (match_char('-')) {
        auto c2 = parse_char_exp();
        if (!c2) {
            return err(std::move(c2.error()));
        }
        return make_char_range_node(flags, c1.value(), c2.value());
    }

    return make_char_node(flags, c1.value());
}

Result<std::optional<RegExp>> RegExp::match_predefined_character_class() {
    // See https://docs.oracle.com/javase/tutorial/essential/regex/pre_char_classes.html
    if (match_char('\\')) {
        if (peek("dDwWsS")) {
            auto code_point = next();
            if (!code_point) {
                return err(std::move(code_point.error()));
            }
            return std::optional<RegExp>(
                    make_leaf_node(flags, RegExpKind::kPreClass, "", 0, 0, 0, 0, code_point.value(), 0));
        }

        if (peek("\\")) {
            auto code_point = next();
            if (!code_point) {
                return err(std::move(code_point.error()));
            }
            return std::optional<RegExp>(make_char_node(flags, code_point.value()));
        }

        // "It is an error to use a backslash prior to any alphabetic character
        // that does not denote an escaped construct."
        if (peek("abcefghijklmnopqrtuvxyz") || peek("ABCEFGHIJKLMNOPQRTUVXYZ")) {
            auto code_point = next();
            if (!code_point) {
                return err(std::move(code_point.error()));
            }
            return err(LuceneError::illegal_argument(
                    std::format("invalid character class \\{}", code_point_text(code_point.value()))));
        }
    }

    return std::optional<RegExp>{};
}

Result<RegExp> RegExp::parse_simple_exp() {
    if (match_char('.')) {
        return make_any_char_node(flags);
    }
    if (check(EMPTY) && match_char('#')) {
        return make_empty_node(flags);
    }
    if (check(ANYSTRING) && match_char('@')) {
        return make_any_string_node(flags);
    }
    if (match_char('"')) {
        const size_t start = pos;
        while (more() && !peek("\"")) {
            auto consumed = next();
            if (!consumed) {
                return err(std::move(consumed.error()));
            }
        }
        if (!match_char('"')) {
            return err(LuceneError::illegal_argument(std::format("expected '\"' at position {}", pos)));
        }
        return make_string_node(flags, original_string.substr(start, pos - 1 - start));
    }
    if (match_char('(')) {
        if (match_char(')')) {
            return make_string_node(flags, "");
        }
        auto e = parse_union_exp();
        if (!e) {
            return err(std::move(e.error()));
        }
        if (!match_char(')')) {
            return err(LuceneError::illegal_argument(std::format("expected ')' at position {}", pos)));
        }
        return e;
    }
    if ((check(AUTOMATON) || check(INTERVAL)) && match_char('<')) {
        const size_t start = pos;
        while (more() && !peek(">")) {
            auto consumed = next();
            if (!consumed) {
                return err(std::move(consumed.error()));
            }
        }
        if (!match_char('>')) {
            return err(LuceneError::illegal_argument(std::format("expected '>' at position {}", pos)));
        }
        const std::string text = original_string.substr(start, pos - 1 - start);
        if (const size_t dash = text.find('-'); dash != std::string::npos) {
            if (!check(INTERVAL)) {
                return err(LuceneError::illegal_argument(std::format("illegal identifier at position {}", pos - 1)));
            }
            if (dash == 0 || dash == text.size() - 1 || text.rfind('-') != dash) {
                return err(LuceneError::illegal_argument(std::format("interval syntax error at position {}", pos - 1)));
            }
            const std::string_view min_text(text.data(), dash);
            const std::string_view max_text(text.data() + dash + 1, text.size() - dash - 1);
            int32_t parsed_min = 0;
            int32_t parsed_max = 0;
            if (!parse_non_negative_int(min_text, parsed_min) || !parse_non_negative_int(max_text, parsed_max)) {
                return err(LuceneError::illegal_argument(std::format("interval syntax error at position {}", pos - 1)));
            }
            const int32_t digits = min_text.size() == max_text.size() ? static_cast<int32_t>(min_text.size()) : 0;
            const int32_t low = std::min(parsed_min, parsed_max);
            const int32_t high = std::max(parsed_min, parsed_max);
            return make_leaf_node(flags, RegExpKind::kInterval, "", 0, low, high, digits, 0, 0);
        }
        if (!check(AUTOMATON)) {
            return err(LuceneError::illegal_argument(std::format("interval syntax error at position {}", pos - 1)));
        }
        return make_leaf_node(flags, RegExpKind::kAutomaton, text, 0, 0, 0, 0, 0, 0);
    }

    auto predefined = match_predefined_character_class();
    if (!predefined) {
        return err(std::move(predefined.error()));
    }
    if (predefined->has_value()) {
        return std::move(predefined->value());
    }
    auto ch = parse_char_exp();
    if (!ch) {
        return err(std::move(ch.error()));
    }
    return make_char_node(flags, ch.value());
}

Result<int32_t> RegExp::parse_char_exp() {
    (void)match_char('\\');
    return next();
}

Result<RegExp> RegExp::expand_predefined() const {
    switch (from) {
    case 'd':
        return from_string("[0-9]");
    case 'D':
        return from_string("[^0-9]");
    case 's':
        return from_string("[ \t\n\r]");
    case 'S':
        return from_string("[^\\s]");
    case 'w':
        return from_string("[a-zA-Z_0-9]");
    case 'W':
        return from_string("[^\\w]");
    default:
        if (from < 0 || from > MAX_CODE_POINT) {
            return err(LuceneError::illegal_argument("invalid unicode value in .from"));
        }
        return err(LuceneError::illegal_argument(
                std::format("invalid character class: \\{}", code_point_text(from))));
    }
}

// ---------------------------------------------------------------------------
// conversion to automata
// ---------------------------------------------------------------------------

Result<Automaton> RegExp::to_automaton() const {
    static const std::map<std::string, Automaton> empty_automata;
    const DefaultProvider provider;
    return to_automaton_impl(empty_automata, provider);
}

Result<Automaton> RegExp::to_automaton_from_map(const std::map<std::string, Automaton> &automata) const {
    const DefaultProvider provider;
    return to_automaton_impl(automata, provider);
}

Result<Automaton> RegExp::to_automaton_from_provider(const AutomatonProvider &provider) const {
    static const std::map<std::string, Automaton> empty_automata;
    return to_automaton_impl(empty_automata, provider);
}

Result<Automaton> RegExp::to_automaton_impl(const std::map<std::string, Automaton> &automata,
                                            const AutomatonProvider &provider) const {
    switch (kind) {
    case RegExpKind::kPreClass: {
        auto expanded = expand_predefined();
        if (!expanded) {
            return err(std::move(expanded.error()));
        }
        return expanded.value().to_automaton_impl(automata, provider);
    }

    case RegExpKind::kUnion:
    case RegExpKind::kConcatenation: {
        std::vector<Automaton> list;
        if (exp1 != nullptr) {
            if (auto leaves = exp1->find_leaves(kind, list, automata, provider); !leaves) {
                return err(std::move(leaves.error()));
            }
        }
        if (exp2 != nullptr) {
            if (auto leaves = exp2->find_leaves(kind, list, automata, provider); !leaves) {
                return err(std::move(leaves.error()));
            }
        }
        std::vector<const Automaton *> automata_list;
        automata_list.reserve(list.size());
        for (const Automaton &a : list) {
            automata_list.push_back(&a);
        }
        return kind == RegExpKind::kUnion ? Operations::union_list(automata_list)
                                          : Operations::concatenate_list(automata_list);
    }

    case RegExpKind::kIntersection: {
        if (exp1 == nullptr || exp2 == nullptr) {
            return err(LuceneError::illegal_state("intersection expression is missing"));
        }
        auto a1 = exp1->to_automaton_impl(automata, provider);
        if (!a1) {
            return err(std::move(a1.error()));
        }
        auto a2 = exp2->to_automaton_impl(automata, provider);
        if (!a2) {
            return err(std::move(a2.error()));
        }
        auto intersection = Operations::intersection(a1.value(), a2.value());
        if (!intersection) {
            return err(std::move(intersection.error()));
        }
        return intersection->has_value() ? std::move(intersection->value()) : std::move(a1.value());
    }

    case RegExpKind::kOptional: {
        if (exp1 == nullptr) {
            return err(LuceneError::illegal_state("optional expression is missing"));
        }
        auto a1 = exp1->to_automaton_impl(automata, provider);
        if (!a1) {
            return err(std::move(a1.error()));
        }
        auto optional = Operations::optional(a1.value());
        if (!optional) {
            return err(std::move(optional.error()));
        }
        return optional->has_value() ? std::move(optional->value()) : std::move(a1.value());
    }

    case RegExpKind::kRepeat: {
        if (exp1 == nullptr) {
            return err(LuceneError::illegal_state("repeat expression is missing"));
        }
        auto a1 = exp1->to_automaton_impl(automata, provider);
        if (!a1) {
            return err(std::move(a1.error()));
        }
        auto repeated = Operations::repeat(a1.value());
        if (!repeated) {
            return err(std::move(repeated.error()));
        }
        return repeated->has_value() ? std::move(repeated->value()) : std::move(a1.value());
    }

    case RegExpKind::kRepeatMin: {
        if (exp1 == nullptr) {
            return err(LuceneError::illegal_state("repeat expression is missing"));
        }
        auto a1 = exp1->to_automaton_impl(automata, provider);
        if (!a1) {
            return err(std::move(a1.error()));
        }
        auto repeated = Operations::repeat_count(a1.value(), min);
        if (!repeated) {
            return err(std::move(repeated.error()));
        }
        return repeated->has_value() ? std::move(repeated->value()) : std::move(a1.value());
    }

    case RegExpKind::kRepeatMinMax: {
        if (exp1 == nullptr) {
            return err(LuceneError::illegal_state("bounded repeat expression is missing"));
        }
        auto a1 = exp1->to_automaton_impl(automata, provider);
        if (!a1) {
            return err(std::move(a1.error()));
        }
        return Operations::repeat_min_max(a1.value(), min, max);
    }

    case RegExpKind::kComplement: {
        // Arbitrary complement is not supported, just negated character classes:
        // a list of characters (e.g. "a") or ranges (e.g. "b-d").
        if (exp1 == nullptr) {
            return err(LuceneError::illegal_state("complement expression is missing"));
        }
        auto a1 = exp1->to_automaton_impl(automata, provider);
        if (!a1) {
            return err(std::move(a1.error()));
        }
        return Operations::complement(a1.value(), static_cast<size_t>(std::numeric_limits<int32_t>::max()));
    }

    case RegExpKind::kDeprecatedComplement: {
        // Arbitrary complement, but bounded by the default work limit: it must
        // not be configurable.
        if (exp1 == nullptr) {
            return err(LuceneError::illegal_state("complement expression is missing"));
        }
        auto a1 = exp1->to_automaton_impl(automata, provider);
        if (!a1) {
            return err(std::move(a1.error()));
        }
        return Operations::complement(a1.value(), Operations::DEFAULT_DETERMINIZE_WORK_LIMIT);
    }

    case RegExpKind::kChar:
        return check(ASCII_CASE_INSENSITIVE) ? to_case_insensitive_char(c) : Automata::make_char(c);

    case RegExpKind::kCharRange:
        return Automata::make_char_range(from, to);

    case RegExpKind::kAnyChar:
        return Automata::make_any_char();

    case RegExpKind::kEmpty:
        return Automata::make_empty();

    case RegExpKind::kString:
        return check(ASCII_CASE_INSENSITIVE) ? to_case_insensitive_string() : Automata::make_string(s);

    case RegExpKind::kAnyString:
        return Automata::make_any_string();

    case RegExpKind::kAutomaton: {
        const auto found = automata.find(s);
        if (found != automata.end()) {
            return found->second;
        }
        auto provided = provider.get_automaton(s);
        if (!provided) {
            return err(std::move(provided.error()));
        }
        if (provided->has_value()) {
            return std::move(provided->value());
        }
        return err(LuceneError::illegal_argument(std::format("'{}' not found ", s)));
    }

    case RegExpKind::kInterval:
        return Automata::make_decimal_interval(min, max, digits);
    }

    return err(LuceneError::illegal_state("unknown regular expression kind"));
}

VoidResult RegExp::find_leaves(RegExpKind target, std::vector<Automaton> &list,
                               const std::map<std::string, Automaton> &automata,
                               const AutomatonProvider &provider) const {
    if (kind == target) {
        if (exp1 != nullptr) {
            if (auto leaves = exp1->find_leaves(target, list, automata, provider); !leaves) {
                return leaves;
            }
        }
        if (exp2 != nullptr) {
            if (auto leaves = exp2->find_leaves(target, list, automata, provider); !leaves) {
                return leaves;
            }
        }
        return {};
    }

    auto automaton = to_automaton_impl(automata, provider);
    if (!automaton) {
        return err(std::move(automaton.error()));
    }
    list.push_back(std::move(automaton.value()));
    return {};
}

Result<Automaton> RegExp::to_case_insensitive_char(int32_t codepoint) {
    auto case1 = Automata::make_char(codepoint);
    if (!case1) {
        return err(std::move(case1.error()));
    }

    if (codepoint > 128) {
        return std::move(case1.value());
    }

    const auto byte = static_cast<uint8_t>(codepoint);
    int32_t alt_case = byte;
    if (byte >= 'a' && byte <= 'z') {
        alt_case = byte - 'a' + 'A';
    } else if (byte >= 'A' && byte <= 'Z') {
        alt_case = byte - 'A' + 'a';
    }

    if (alt_case == codepoint) {
        return std::move(case1.value());
    }

    auto case2 = Automata::make_char(alt_case);
    if (!case2) {
        return err(std::move(case2.error()));
    }
    return Operations::union_with(case1.value(), case2.value());
}

Result<Automaton> RegExp::to_case_insensitive_string() const {
    auto code_points = StringHelper::utf8_to_code_points(s);
    if (!code_points) {
        return err(std::move(code_points.error()));
    }

    std::vector<Automaton> list;
    list.reserve(code_points->size());
    for (const int32_t code_point : code_points.value()) {
        auto automaton = to_case_insensitive_char(code_point);
        if (!automaton) {
            return err(std::move(automaton.error()));
        }
        list.push_back(std::move(automaton.value()));
    }

    std::vector<const Automaton *> automata_list;
    automata_list.reserve(list.size());
    for (const Automaton &a : list) {
        automata_list.push_back(&a);
    }
    return Operations::concatenate_list(automata_list);
}

// ---------------------------------------------------------------------------
// rendering
// ---------------------------------------------------------------------------

VoidResult RegExp::to_string_builder(std::string &b) const {
    switch (kind) {
    case RegExpKind::kUnion:
        b.push_back('(');
        if (auto result = append_child(b, exp1.get()); !result) {
            return result;
        }
        b.push_back('|');
        if (auto result = append_child(b, exp2.get()); !result) {
            return result;
        }
        b.push_back(')');
        break;
    case RegExpKind::kConcatenation:
        if (auto result = append_child(b, exp1.get()); !result) {
            return result;
        }
        if (auto result = append_child(b, exp2.get()); !result) {
            return result;
        }
        break;
    case RegExpKind::kIntersection:
        b.push_back('(');
        if (auto result = append_child(b, exp1.get()); !result) {
            return result;
        }
        b.push_back('&');
        if (auto result = append_child(b, exp2.get()); !result) {
            return result;
        }
        b.push_back(')');
        break;
    case RegExpKind::kOptional:
        b.push_back('(');
        if (auto result = append_child(b, exp1.get()); !result) {
            return result;
        }
        b += ")?";
        break;
    case RegExpKind::kRepeat:
        b.push_back('(');
        if (auto result = append_child(b, exp1.get()); !result) {
            return result;
        }
        b += ")*";
        break;
    case RegExpKind::kRepeatMin:
        b.push_back('(');
        if (auto result = append_child(b, exp1.get()); !result) {
            return result;
        }
        b += "){";
        b += std::to_string(min);
        b += ",}";
        break;
    case RegExpKind::kRepeatMinMax:
        b.push_back('(');
        if (auto result = append_child(b, exp1.get()); !result) {
            return result;
        }
        b += "){";
        b += std::to_string(min);
        b.push_back(',');
        b += std::to_string(max);
        b.push_back('}');
        break;
    case RegExpKind::kComplement:
    case RegExpKind::kDeprecatedComplement:
        b += "~(";
        if (auto result = append_child(b, exp1.get()); !result) {
            return result;
        }
        b.push_back(')');
        break;
    case RegExpKind::kChar:
        b.push_back('\\');
        append_code_point(b, c);
        break;
    case RegExpKind::kCharRange:
        b += "[\\";
        b += code_point_text_or_unknown(from);
        b.push_back('-');
        b.push_back('\\');
        b += code_point_text_or_unknown(to);
        b.push_back(']');
        break;
    case RegExpKind::kAnyChar:
        b.push_back('.');
        break;
    case RegExpKind::kEmpty:
        b.push_back('#');
        break;
    case RegExpKind::kString:
        b.push_back('"');
        b += s;
        b.push_back('"');
        break;
    case RegExpKind::kAnyString:
        b.push_back('@');
        break;
    case RegExpKind::kAutomaton:
        b.push_back('<');
        b += s;
        b.push_back('>');
        break;
    case RegExpKind::kInterval: {
        const std::string s1 = std::to_string(min);
        const std::string s2 = std::to_string(max);
        b.push_back('<');
        if (digits > 0) {
            for (size_t i = s1.size(); i < static_cast<size_t>(digits); ++i) {
                b.push_back('0');
            }
        }
        b += s1;
        b.push_back('-');
        if (digits > 0) {
            for (size_t i = s2.size(); i < static_cast<size_t>(digits); ++i) {
                b.push_back('0');
            }
        }
        b += s2;
        b.push_back('>');
        break;
    }
    case RegExpKind::kPreClass:
        b.push_back('\\');
        append_code_point(b, from);
        break;
    }
    return {};
}

Result<std::string> RegExp::to_string() const {
    std::string b;
    if (auto result = to_string_builder(b); !result) {
        return err(std::move(result.error()));
    }
    return b;
}

Result<std::string> RegExp::to_string_tree() const {
    std::string b;
    if (auto result = to_string_tree_from_string(b, ""); !result) {
        return err(std::move(result.error()));
    }
    return b;
}

VoidResult RegExp::to_string_tree_from_string(std::string &b, std::string_view indent) const {
    const std::string indent_more = std::string(indent) + "  ";

    switch (kind) {
    case RegExpKind::kUnion:
    case RegExpKind::kConcatenation:
    case RegExpKind::kIntersection:
        b += indent;
        b += std::format("{}\n", kind_name(kind));
        if (exp1 != nullptr) {
            if (auto result = exp1->to_string_tree_from_string(b, indent_more); !result) {
                return result;
            }
        }
        if (exp2 != nullptr) {
            if (auto result = exp2->to_string_tree_from_string(b, indent_more); !result) {
                return result;
            }
        }
        break;

    case RegExpKind::kOptional:
    case RegExpKind::kRepeat:
    case RegExpKind::kComplement:
    case RegExpKind::kDeprecatedComplement:
        b += indent;
        b += std::format("{}\n", kind_name(kind));
        if (exp1 != nullptr) {
            if (auto result = exp1->to_string_tree_from_string(b, indent_more); !result) {
                return result;
            }
        }
        break;

    case RegExpKind::kRepeatMin:
        b += indent;
        b += std::format("{} min={}\n", kind_name(kind), min);
        if (exp1 != nullptr) {
            if (auto result = exp1->to_string_tree_from_string(b, indent_more); !result) {
                return result;
            }
        }
        break;

    case RegExpKind::kRepeatMinMax:
        b += indent;
        b += std::format("{} min={} max={}\n", kind_name(kind), min, max);
        if (exp1 != nullptr) {
            if (auto result = exp1->to_string_tree_from_string(b, indent_more); !result) {
                return result;
            }
        }
        break;

    case RegExpKind::kChar:
        b += indent;
        b += std::format("{} char={}\n", kind_name(kind), code_point_text(c));
        break;

    case RegExpKind::kPreClass:
        b += indent;
        b += std::format("{} class=\\{}\n", kind_name(kind), code_point_text(from));
        break;

    case RegExpKind::kCharRange:
        b += indent;
        b += std::format("{} from={} to={}\n", kind_name(kind), code_point_text_or_unknown(from),
                         code_point_text_or_unknown(to));
        break;

    case RegExpKind::kString:
        b += indent;
        b += std::format("{} string={}\n", kind_name(kind), s);
        break;

    case RegExpKind::kInterval: {
        b += indent;
        b += std::format("{}<", kind_name(kind));
        std::string s1 = std::to_string(min);
        std::string s2 = std::to_string(max);
        if (digits > 0) {
            for (size_t i = s1.size(); i < static_cast<size_t>(digits); ++i) {
                b.push_back('0');
            }
        }
        b += s1;
        b.push_back('-');
        if (digits > 0) {
            for (size_t i = s2.size(); i < static_cast<size_t>(digits); ++i) {
                b.push_back('0');
            }
        }
        b += s2;
        b += ">\n";
        break;
    }

    case RegExpKind::kAnyChar:
    case RegExpKind::kAnyString:
    case RegExpKind::kEmpty:
    case RegExpKind::kAutomaton:
        b += indent;
        b += std::format("{}\n", kind_name(kind));
        break;
    }

    return {};
}

std::set<std::string> RegExp::get_identifiers_set() const {
    std::set<std::string> set;
    get_identifiers(set);
    return set;
}

void RegExp::get_identifiers(std::set<std::string> &set) const {
    switch (kind) {
    case RegExpKind::kUnion:
    case RegExpKind::kConcatenation:
    case RegExpKind::kIntersection:
        if (exp1 != nullptr) {
            exp1->get_identifiers(set);
        }
        if (exp2 != nullptr) {
            exp2->get_identifiers(set);
        }
        break;
    case RegExpKind::kOptional:
    case RegExpKind::kRepeat:
    case RegExpKind::kRepeatMin:
    case RegExpKind::kRepeatMinMax:
    case RegExpKind::kComplement:
    case RegExpKind::kDeprecatedComplement:
        if (exp1 != nullptr) {
            exp1->get_identifiers(set);
        }
        break;
    case RegExpKind::kAutomaton:
        set.insert(s);
        break;
    case RegExpKind::kAnyChar:
    case RegExpKind::kAnyString:
    case RegExpKind::kChar:
    case RegExpKind::kCharRange:
    case RegExpKind::kEmpty:
    case RegExpKind::kInterval:
    case RegExpKind::kPreClass:
    case RegExpKind::kString:
        break;
    }
}

} // namespace lucene
