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

module lucene_core:query_parser.impl;

import :query_parser;

import :analyzer;
import :boolean_clause;
import :error;
import :occur;
import :query;
import :query_parser_base;
import :query_parser_constants;
import :query_parser_token;
import :types;

import std;

namespace lucene {

namespace {

/// `disallowedPostMultiTerm` of the reference implementation: a following term
/// that is glued to one of these tokens starts its own clause.
[[nodiscard]] bool allowed_post_multi_term(int32_t kind) noexcept {
    switch (kind) {
        case query_syntax::COLON:
        case query_syntax::STAR:
        case query_syntax::FUZZY_SLOP:
        case query_syntax::CARAT:
        case query_syntax::AND:
        case query_syntax::OR:
            return false;
        default:
            return true;
    }
}

} // namespace

QueryParser::QueryParser(std::string default_field, Analyzer &analyzer) : QueryParserBase(analyzer) {
    set_field(std::move(default_field));
}

// ---------------------------------------------------------------------------
// Token helpers
// ---------------------------------------------------------------------------

const QueryToken &QueryParser::peek(size_t offset) const {
    const size_t index = position_ + offset;
    return index < tokens_.size() ? tokens_[index] : tokens_.back();
}

bool QueryParser::accept(int32_t kind) {
    if (peek().kind != kind) {
        return false;
    }
    ++position_;
    return true;
}

const QueryToken &QueryParser::advance() {
    const QueryToken &token = peek();
    if (position_ + 1 < tokens_.size()) {
        ++position_;
    }
    return token;
}

LuceneError QueryParser::unexpected_token(std::string_view expectation) const {
    const QueryToken &token = peek();
    std::string found = token.kind == query_syntax::EOF_TOKEN ? "<EOF>" : token.image;
    std::string message = "Encountered \"" + found + "\" at line " + std::to_string(token.begin_line) +
                          ", column " + std::to_string(token.begin_column);
    if (!expectation.empty()) {
        message += ", was expecting " + std::string(expectation);
    }
    message += ".";
    return LuceneError::parse(std::move(message));
}

VoidResult QueryParser::expect(int32_t kind) {
    if (!accept(kind)) {
        return std::unexpected(unexpected_token("\"" + query_syntax::token_image(kind) + "\""));
    }
    return {};
}

const QueryToken *QueryParser::take(int32_t kind) {
    if (peek().kind != kind) {
        return nullptr;
    }
    const QueryToken *token = &peek();
    ++position_;
    return token;
}

Result<const QueryToken *> QueryParser::take_number() {
    if (peek().kind != query_syntax::NUMBER) {
        return std::unexpected(unexpected_token("a number"));
    }
    const QueryToken *token = &peek();
    ++position_;
    return token;
}

bool QueryParser::is_multi_term_start() const {
    // `LOOKAHEAD(2) MultiTerm(...)`: the production starts with two `TERM`s, so
    // two tokens decide — a lone term, or `field:`, takes the `Clause` branch.
    return peek(0).kind == query_syntax::TERM && peek(1).kind == query_syntax::TERM;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

Result<std::shared_ptr<Query>> QueryParser::parse(std::string_view query) {
    auto tokenized = tokenize_query(query);
    if (!tokenized.has_value()) {
        return std::unexpected(std::move(tokenized.error()));
    }
    tokens_ = std::move(tokenized.value());
    position_ = 0;

    auto parsed = parse_query(field());
    if (!parsed.has_value()) {
        return std::unexpected(std::move(parsed.error()));
    }
    // `TopLevelQuery ::= Query <EOF>`
    if (peek().kind != query_syntax::EOF_TOKEN) {
        return std::unexpected(unexpected_token("<EOF>"));
    }
    return parsed;
}

// ---------------------------------------------------------------------------
// Productions
// ---------------------------------------------------------------------------

Result<std::shared_ptr<Query>> QueryParser::parse_query(std::string_view field) {
    std::vector<BooleanClause> clauses;
    std::shared_ptr<Query> first_query;

    if (is_multi_term_start()) {
        auto parsed = parse_multi_term(field, clauses, first_query);
        if (!parsed.has_value()) {
            return std::unexpected(std::move(parsed.error()));
        }
    } else {
        const Modifier mods = parse_modifiers();
        auto clause = parse_clause(field);
        if (!clause.has_value()) {
            return std::unexpected(std::move(clause.error()));
        }
        if (VoidResult added = add_clause(clauses, Conjunction::kNone, mods, clause.value()); !added.has_value()) {
            return std::unexpected(std::move(added.error()));
        }
        if (mods == Modifier::kNone) {
            first_query = clause.value();
        }
    }

    while (peek().kind != query_syntax::EOF_TOKEN && peek().kind != query_syntax::RPAREN) {
        if (is_multi_term_start()) {
            std::shared_ptr<Query> ignored;
            auto parsed = parse_multi_term(field, clauses, ignored);
            if (!parsed.has_value()) {
                return std::unexpected(std::move(parsed.error()));
            }
            continue;
        }

        const Conjunction conj = parse_conjunction();
        const Modifier mods = parse_modifiers();
        auto clause = parse_clause(field);
        if (!clause.has_value()) {
            return std::unexpected(std::move(clause.error()));
        }
        if (VoidResult added = add_clause(clauses, conj, mods, clause.value()); !added.has_value()) {
            return std::unexpected(std::move(added.error()));
        }
    }

    if (clauses.size() == 1 && first_query != nullptr) {
        return first_query;
    }
    return get_boolean_query(std::move(clauses));
}

Result<std::shared_ptr<Query>> QueryParser::parse_clause(std::string_view field) {
    std::string clause_field(field);
    const QueryToken *boost = nullptr;

    // Optional `field:` prefix.
    if (peek(0).kind == query_syntax::TERM && peek(1).kind == query_syntax::COLON) {
        auto unescaped = discard_escape_char(peek().image);
        if (!unescaped.has_value()) {
            return std::unexpected(std::move(unescaped.error()));
        }
        clause_field = std::move(unescaped.value());
        position_ += 2;
    } else if (peek(0).kind == query_syntax::STAR && peek(1).kind == query_syntax::COLON) {
        clause_field = "*";
        position_ += 2;
    }

    std::shared_ptr<Query> query;
    if (accept(query_syntax::LPAREN)) {
        auto nested = parse_query(clause_field);
        if (!nested.has_value()) {
            return std::unexpected(std::move(nested.error()));
        }
        if (VoidResult closed = expect(query_syntax::RPAREN); !closed.has_value()) {
            return std::unexpected(std::move(closed.error()));
        }
        if (peek().kind == query_syntax::CARAT) {
            ++position_;
            auto number = take_number();
            if (!number.has_value()) {
                return std::unexpected(std::move(number.error()));
            }
            boost = number.value();
        }
        query = std::move(nested.value());
    } else {
        auto term = parse_term(clause_field);
        if (!term.has_value()) {
            return std::unexpected(std::move(term.error()));
        }
        query = std::move(term.value());
    }
    return handle_boost(std::move(query), boost);
}

Result<std::shared_ptr<Query>> QueryParser::parse_term(std::string_view field) {
    const QueryToken *boost = nullptr;
    const QueryToken *fuzzy_slop = nullptr;
    bool prefix = false;
    bool wildcard = false;
    bool fuzzy = false;
    bool regexp = false;

    switch (peek().kind) {
        case query_syntax::RANGEIN_START:
        case query_syntax::RANGEEX_START: {
            bool start_inclusive = false;
            if (accept(query_syntax::RANGEIN_START)) {
                start_inclusive = true;
            } else if (VoidResult opened = expect(query_syntax::RANGEEX_START); !opened.has_value()) {
                return std::unexpected(std::move(opened.error()));
            }

            const QueryToken &lower_token = advance();
            if (VoidResult to = expect(query_syntax::RANGE_TO); !to.has_value()) {
                return std::unexpected(std::move(to.error()));
            }
            const QueryToken &upper_token = advance();

            bool end_inclusive = false;
            if (accept(query_syntax::RANGEIN_END)) {
                end_inclusive = true;
            } else if (VoidResult closed = expect(query_syntax::RANGEEX_END); !closed.has_value()) {
                return std::unexpected(std::move(closed.error()));
            }
            if (peek().kind == query_syntax::CARAT) {
                ++position_;
                auto number = take_number();
                if (!number.has_value()) {
                    return std::unexpected(std::move(number.error()));
                }
                boost = number.value();
            }

            // A quoted bound keeps its escapes, an unquoted one is unescaped, and
            // `*` opens the end.
            const auto bound = [](const QueryToken &token) -> Result<std::optional<std::string>> {
                if (token.kind == query_syntax::RANGE_QUOTED) {
                    return std::optional<std::string>(token.image.substr(1, token.image.size() - 2));
                }
                if (token.image == "*") {
                    return std::optional<std::string>{};
                }
                auto unescaped = discard_escape_char(token.image);
                if (!unescaped.has_value()) {
                    return std::unexpected(std::move(unescaped.error()));
                }
                return std::optional<std::string>(std::move(unescaped.value()));
            };

            auto lower = bound(lower_token);
            if (!lower.has_value()) {
                return std::unexpected(std::move(lower.error()));
            }
            auto upper = bound(upper_token);
            if (!upper.has_value()) {
                return std::unexpected(std::move(upper.error()));
            }

            auto query = get_range_query(field, std::move(lower.value()), std::move(upper.value()), start_inclusive,
                                         end_inclusive);
            if (!query.has_value()) {
                return std::unexpected(std::move(query.error()));
            }
            return handle_boost(std::move(query.value()), boost);
        }

        case query_syntax::QUOTED: {
            const QueryToken quoted = advance();
            const QueryToken *quoted_boost = nullptr;
            const QueryToken *quoted_slop = nullptr;
            // The grammar allows the boost and the slop in either order:
            // `"a b"^2~3` and `"a b"~3^2` are both valid.
            if (peek().kind == query_syntax::CARAT) {
                ++position_;
                auto number = take_number();
                if (!number.has_value()) {
                    return std::unexpected(std::move(number.error()));
                }
                quoted_boost = number.value();
                quoted_slop = take(query_syntax::FUZZY_SLOP);
            } else {
                quoted_slop = take(query_syntax::FUZZY_SLOP);
                if (quoted_slop != nullptr && peek().kind == query_syntax::CARAT) {
                    ++position_;
                    auto number = take_number();
                    if (!number.has_value()) {
                        return std::unexpected(std::move(number.error()));
                    }
                    quoted_boost = number.value();
                }
            }
            auto query = handle_quoted_term(field, quoted, quoted_slop);
            if (!query.has_value()) {
                return std::unexpected(std::move(query.error()));
            }
            return handle_boost(std::move(query.value()), quoted_boost);
        }

        default:
            break;
    }

    QueryToken token;
    switch (peek().kind) {
        case query_syntax::TERM:
            token = advance();
            break;
        case query_syntax::STAR:
            token = advance();
            wildcard = true;
            break;
        case query_syntax::PREFIXTERM:
            token = advance();
            prefix = true;
            break;
        case query_syntax::WILDTERM:
            token = advance();
            wildcard = true;
            break;
        case query_syntax::REGEXPTERM:
            token = advance();
            regexp = true;
            break;
        case query_syntax::NUMBER:
            token = advance();
            break;
        case query_syntax::BAREOPER:
            token = advance();
            // A bare `+`/`-`/`!` is a single character term; the blank it carried
            // is not part of it.
            token.image = token.image.substr(0, 1);
            break;
        default:
            return std::unexpected(unexpected_token("a term"));
    }

    if (peek().kind == query_syntax::CARAT) {
        ++position_;
        auto number = take_number();
        if (!number.has_value()) {
            return std::unexpected(std::move(number.error()));
        }
        boost = number.value();
        if (const QueryToken *slop = take(query_syntax::FUZZY_SLOP); slop != nullptr) {
            fuzzy_slop = slop;
            fuzzy = true;
        }
    } else if (const QueryToken *slop = take(query_syntax::FUZZY_SLOP); slop != nullptr) {
        fuzzy_slop = slop;
        fuzzy = true;
        if (peek().kind == query_syntax::CARAT) {
            ++position_;
            auto number = take_number();
            if (!number.has_value()) {
                return std::unexpected(std::move(number.error()));
            }
            boost = number.value();
        }
    }

    auto query = handle_bare_token_query(field, token, fuzzy_slop, prefix, wildcard, fuzzy, regexp);
    if (!query.has_value()) {
        return std::unexpected(std::move(query.error()));
    }
    return handle_boost(std::move(query.value()), boost);
}

Result<std::shared_ptr<Query>> QueryParser::parse_multi_term(std::string_view field,
                                                             std::vector<BooleanClause> &clauses,
                                                             std::shared_ptr<Query> &first_query) {
    std::string image = advance().image;

    if (split_on_whitespace()) {
        auto unescaped = discard_escape_char(image);
        if (!unescaped.has_value()) {
            return std::unexpected(std::move(unescaped.error()));
        }
        auto query = get_field_query(field, unescaped.value(), false);
        if (!query.has_value()) {
            return std::unexpected(std::move(query.error()));
        }
        if (VoidResult added = add_clause(clauses, Conjunction::kNone, Modifier::kNone, query.value());
            !added.has_value()) {
            return std::unexpected(std::move(added.error()));
        }
        first_query = query.value();
    }

    while (peek(0).kind == query_syntax::TERM && allowed_post_multi_term(peek(1).kind)) {
        const QueryToken following = advance();
        if (split_on_whitespace()) {
            auto unescaped = discard_escape_char(following.image);
            if (!unescaped.has_value()) {
                return std::unexpected(std::move(unescaped.error()));
            }
            auto query = get_field_query(field, unescaped.value(), false);
            if (!query.has_value()) {
                return std::unexpected(std::move(query.error()));
            }
            if (VoidResult added = add_clause(clauses, Conjunction::kNone, Modifier::kNone, query.value());
                !added.has_value()) {
                return std::unexpected(std::move(added.error()));
            }
        } else {
            image += ' ';
            image += following.image;
        }
    }

    if (!split_on_whitespace()) {
        auto unescaped = discard_escape_char(image);
        if (!unescaped.has_value()) {
            return std::unexpected(std::move(unescaped.error()));
        }
        auto query = get_field_query(field, unescaped.value(), false);
        if (!query.has_value()) {
            return std::unexpected(std::move(query.error()));
        }
        if (VoidResult added = add_multi_term_clauses(clauses, query.value()); !added.has_value()) {
            return std::unexpected(std::move(added.error()));
        }
        first_query = query.value();
    }
    return first_query;
}

Conjunction QueryParser::parse_conjunction() {
    if (accept(query_syntax::AND)) {
        return Conjunction::kAnd;
    }
    if (accept(query_syntax::OR)) {
        return Conjunction::kOr;
    }
    return Conjunction::kNone;
}

Modifier QueryParser::parse_modifiers() {
    if (accept(query_syntax::PLUS)) {
        return Modifier::kRequired;
    }
    if (accept(query_syntax::MINUS) || accept(query_syntax::NOT)) {
        return Modifier::kNot;
    }
    return Modifier::kNone;
}

} // namespace lucene
