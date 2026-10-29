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

module lucene_core:query_parser_base.impl;

import :query_parser_base;

import :analyzer;
import :attributes;
import :automaton_provider;
import :boolean_clause;
import :boolean_query;
import :boost_query;
import :bytes_ref;
import :error;
import :fuzzy_query;
import :match_all_docs_query;
import :multi_phrase_query;
import :multi_term_query;
import :occur;
import :phrase_query;
import :prefix_query;
import :query;
import :query_builder;
import :query_parser_token;
import :reg_exp;
import :regexp_query;
import :term;
import :term_query;
import :term_range_query;
import :token_stream;
import :types;
import :wildcard_query;

import std;

namespace lucene {

namespace {

/// `Float.parseFloat` returns the value or fails; this reports whether `text`
/// parsed completely.
std::optional<float> parse_float(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::string buffer(text);
    char *end = nullptr;
    const float value = std::strtof(buffer.c_str(), &end);
    if (end != buffer.c_str() + buffer.size()) {
        return std::nullopt;
    }
    return value;
}

} // namespace

QueryParserBase::QueryParserBase(Analyzer &analyzer) : QueryBuilder(analyzer) {}

VoidResult QueryParserBase::set_split_on_whitespace(bool value) {
    if (!value && auto_generate_phrase_queries_) {
        return err(LuceneError::illegal_argument(
            "set_split_on_whitespace(false) is disallowed when auto_generate_phrase_queries is on"));
    }
    split_on_whitespace_ = value;
    return {};
}

std::string QueryParserBase::escape(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char current : text) {
        switch (current) {
            case '\\':
            case '+':
            case '-':
            case '!':
            case '(':
            case ')':
            case ':':
            case '^':
            case '[':
            case ']':
            case '"':
            case '{':
            case '}':
            case '~':
            case '*':
            case '?':
            case '|':
            case '&':
            case '/':
                escaped.push_back('\\');
                break;
            default:
                break;
        }
        escaped.push_back(current);
    }
    return escaped;
}

// ---------------------------------------------------------------------------
// Query construction
// ---------------------------------------------------------------------------

Result<std::shared_ptr<Query>> QueryParserBase::get_field_query(std::string_view field, std::string_view query_text,
                                                                bool quoted) {
    const Occur occur = default_operator_ == QueryParserOperator::kAnd ? Occur::Must : Occur::Should;
    return create_field_query(occur, field, query_text, quoted || auto_generate_phrase_queries_, phrase_slop_);
}

Result<std::shared_ptr<Query>> QueryParserBase::get_field_query_with_slop(std::string_view field,
                                                                          std::string_view query_text, int32_t slop) {
    auto query = get_field_query(field, query_text, true);
    if (!query.has_value()) {
        return std::unexpected(std::move(query.error()));
    }
    std::shared_ptr<Query> built = std::move(query.value());
    if (built == nullptr) {
        return built;
    }

    if (auto *phrase = dynamic_cast<PhraseQuery *>(built.get()); phrase != nullptr) {
        auto rebuilt = PhraseQuery::with_positions(phrase->field(), phrase->terms(), phrase->positions(), slop);
        if (!rebuilt.has_value()) {
            return std::unexpected(std::move(rebuilt.error()));
        }
        return std::shared_ptr<Query>(std::make_shared<PhraseQuery>(std::move(rebuilt.value())));
    }
    if (auto *multi = dynamic_cast<MultiPhraseQuery *>(built.get()); multi != nullptr) {
        if (multi->slop() != slop) {
            MultiPhraseQuery::Builder builder(*multi);
            if (VoidResult set = builder.set_slop(slop); !set.has_value()) {
                return std::unexpected(std::move(set.error()));
            }
            auto rebuilt = builder.build();
            if (!rebuilt.has_value()) {
                return std::unexpected(std::move(rebuilt.error()));
            }
            return std::shared_ptr<Query>(std::make_shared<MultiPhraseQuery>(std::move(rebuilt.value())));
        }
    }
    return built;
}

Result<std::shared_ptr<Query>> QueryParserBase::get_range_query(std::string_view field, std::optional<std::string> part1,
                                                                std::optional<std::string> part2,
                                                                bool start_inclusive, bool end_inclusive) {
    // The reference implementation formats both bounds through `DateTools` when
    // they parse as dates; `DateTools` is not ported, so the bounds stay terms.
    std::optional<BytesRef> lower;
    if (part1.has_value()) {
        auto normalized = normalize(field, part1.value());
        if (!normalized.has_value()) {
            return std::unexpected(std::move(normalized.error()));
        }
        lower = std::move(normalized.value());
    }
    std::optional<BytesRef> upper;
    if (part2.has_value()) {
        auto normalized = normalize(field, part2.value());
        if (!normalized.has_value()) {
            return std::unexpected(std::move(normalized.error()));
        }
        upper = std::move(normalized.value());
    }
    return new_range_query(std::string(field), std::move(lower), std::move(upper), start_inclusive, end_inclusive);
}

Result<std::shared_ptr<Query>> QueryParserBase::get_prefix_query(std::string_view field, std::string_view term_str) {
    if (!allow_leading_wildcard_ && !term_str.empty() && term_str.front() == '*') {
        return err(LuceneError::parse("'*' not allowed as first character in PrefixQuery"));
    }
    auto normalized = normalize(field, term_str);
    if (!normalized.has_value()) {
        return std::unexpected(std::move(normalized.error()));
    }
    return new_prefix_query(Term(std::string(field), std::move(normalized.value())));
}

Result<std::shared_ptr<Query>> QueryParserBase::get_wildcard_query(std::string_view field, std::string_view term_str) {
    if (field == "*" && term_str == "*") {
        return new_match_all_docs_query();
    }
    if (!allow_leading_wildcard_ && !term_str.empty() && (term_str.front() == '*' || term_str.front() == '?')) {
        return err(LuceneError::parse("'*' or '?' not allowed as first character in WildcardQuery"));
    }
    auto analyzed = analyze_wildcard(field, term_str);
    if (!analyzed.has_value()) {
        return std::unexpected(std::move(analyzed.error()));
    }
    return new_wildcard_query(Term(std::string(field), BytesRef::from_string(analyzed.value())));
}

Result<std::shared_ptr<Query>> QueryParserBase::get_regexp_query(std::string_view field, std::string_view term_str) {
    auto normalized = normalize(field, term_str);
    if (!normalized.has_value()) {
        return std::unexpected(std::move(normalized.error()));
    }
    return new_regexp_query(Term(std::string(field), std::move(normalized.value())));
}

Result<std::shared_ptr<Query>> QueryParserBase::get_fuzzy_query(std::string_view field, std::string_view term_str,
                                                                float min_similarity) {
    auto normalized = normalize(field, term_str);
    if (!normalized.has_value()) {
        return std::unexpected(std::move(normalized.error()));
    }
    return new_fuzzy_query(Term(std::string(field), std::move(normalized.value())), min_similarity,
                           fuzzy_prefix_length_);
}

Result<std::shared_ptr<Query>> QueryParserBase::get_boolean_query(std::vector<BooleanClause> clauses) {
    if (clauses.empty()) {
        // Every clause was filtered away by the analyzer.
        return std::shared_ptr<Query>{};
    }
    return new_boolean_query(std::move(clauses));
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

Result<std::shared_ptr<Query>> QueryParserBase::new_prefix_query(const Term &prefix) {
    return std::shared_ptr<Query>(std::make_shared<PrefixQuery>(prefix.field(), prefix.text(), multi_term_rewrite_method_));
}

Result<std::shared_ptr<Query>> QueryParserBase::new_wildcard_query(const Term &wildcard) {
    auto built = WildcardQuery::with_rewrite(wildcard, determinize_work_limit_, multi_term_rewrite_method_);
    if (!built.has_value()) {
        return std::unexpected(std::move(built.error()));
    }
    return std::shared_ptr<Query>(std::make_shared<WildcardQuery>(std::move(built.value())));
}

Result<std::shared_ptr<Query>> QueryParserBase::new_regexp_query(const Term &regexp) {
    const DefaultProvider provider;
    auto built = RegexpQuery::with_all(regexp, RegExp::ALL, 0, provider, determinize_work_limit_,
                                      multi_term_rewrite_method_);
    if (!built.has_value()) {
        return std::unexpected(std::move(built.error()));
    }
    return std::shared_ptr<Query>(std::make_shared<RegexpQuery>(std::move(built.value())));
}

Result<std::shared_ptr<Query>> QueryParserBase::new_fuzzy_query(const Term &term, float min_similarity,
                                                               int32_t prefix_length) {
    const int32_t edits = FuzzyQuery::float_to_edits(min_similarity, term.text().size());
    auto built = FuzzyQuery::with_rewrite(term, edits, static_cast<size_t>(std::max(0, prefix_length)),
                                          FuzzyQuery::DEFAULT_MAX_EXPANSIONS, FuzzyQuery::DEFAULT_TRANSPOSITIONS,
                                          multi_term_rewrite_method_);
    if (!built.has_value()) {
        return std::unexpected(std::move(built.error()));
    }
    return std::shared_ptr<Query>(std::make_shared<FuzzyQuery>(std::move(built.value())));
}

Result<std::shared_ptr<Query>> QueryParserBase::new_range_query(std::string field, std::optional<BytesRef> lower_term,
                                                                std::optional<BytesRef> upper_term,
                                                                bool include_lower, bool include_upper) {
    auto built = TermRangeQuery::with_rewrite(std::move(field), std::move(lower_term), std::move(upper_term),
                                              include_lower, include_upper, multi_term_rewrite_method_);
    if (!built.has_value()) {
        return std::unexpected(std::move(built.error()));
    }
    return std::shared_ptr<Query>(std::make_shared<TermRangeQuery>(std::move(built.value())));
}

std::shared_ptr<Query> QueryParserBase::new_match_all_docs_query() { return MatchAllDocsQuery::create(); }

BooleanClause QueryParserBase::new_boolean_clause(std::shared_ptr<Query> query, Occur occur) {
    return BooleanClause::create(std::move(query), occur);
}

// ---------------------------------------------------------------------------
// Clause plumbing
// ---------------------------------------------------------------------------

VoidResult QueryParserBase::add_clause(std::vector<BooleanClause> &clauses, Conjunction conj, Modifier mods,
                                       std::shared_ptr<Query> q) {
    // A term introduced by AND makes the preceding term required (unless it is
    // already prohibited).
    if (!clauses.empty() && conj == Conjunction::kAnd) {
        BooleanClause &previous = clauses.back();
        if (!previous.is_prohibited()) {
            previous.occur = Occur::Must;
        }
    }

    // With AND as the default operator, a term introduced by OR makes the
    // preceding term optional: `a OR b` would otherwise parse as `+a OR b`.
    if (!clauses.empty() && default_operator_ == QueryParserOperator::kAnd && conj == Conjunction::kOr) {
        BooleanClause &previous = clauses.back();
        if (!previous.is_prohibited()) {
            previous.occur = Occur::Should;
        }
    }

    // The term might have been filtered away by the analyzer.
    if (q == nullptr) {
        return {};
    }

    bool required = false;
    bool prohibited = false;
    if (default_operator_ == QueryParserOperator::kOr) {
        prohibited = mods == Modifier::kNot;
        required = mods == Modifier::kRequired;
        if (conj == Conjunction::kAnd && !prohibited) {
            required = true;
        }
    } else {
        prohibited = mods == Modifier::kNot;
        required = !prohibited && conj != Conjunction::kOr;
    }

    if (required && !prohibited) {
        clauses.push_back(new_boolean_clause(std::move(q), Occur::Must));
    } else if (!required && !prohibited) {
        clauses.push_back(new_boolean_clause(std::move(q), Occur::Should));
    } else if (!required && prohibited) {
        clauses.push_back(new_boolean_clause(std::move(q), Occur::MustNot));
    } else {
        return err(LuceneError::illegal_state("a clause cannot be both required and prohibited"));
    }
    return {};
}

VoidResult QueryParserBase::add_multi_term_clauses(std::vector<BooleanClause> &clauses, std::shared_ptr<Query> q) {
    if (q == nullptr) {
        return {};
    }

    auto *boolean = dynamic_cast<BooleanQuery *>(q.get());
    bool all_nested_term_queries = boolean != nullptr;
    if (boolean != nullptr) {
        for (const BooleanClause &clause : boolean->clauses()) {
            if (dynamic_cast<TermQuery *>(clause.query.get()) == nullptr) {
                all_nested_term_queries = false;
                break;
            }
        }
    }

    if (all_nested_term_queries) {
        const std::vector<BooleanClause> &nested = boolean->clauses();
        clauses.insert(clauses.end(), nested.begin(), nested.end());
        return {};
    }

    const Occur occur = default_operator_ == QueryParserOperator::kOr ? Occur::Should : Occur::Must;
    if (boolean != nullptr) {
        for (const BooleanClause &clause : boolean->clauses()) {
            clauses.push_back(new_boolean_clause(clause.query, occur));
        }
    } else {
        clauses.push_back(new_boolean_clause(std::move(q), occur));
    }
    return {};
}

// ---------------------------------------------------------------------------
// Token handling
// ---------------------------------------------------------------------------

Result<std::shared_ptr<Query>> QueryParserBase::handle_bare_token_query(std::string_view field, const QueryToken &term,
                                                                       const QueryToken *fuzzy_slop, bool prefix,
                                                                       bool wildcard, bool fuzzy, bool regexp) {
    auto term_image = discard_escape_char(term.image);
    if (!term_image.has_value()) {
        return std::unexpected(std::move(term_image.error()));
    }

    if (wildcard) {
        return get_wildcard_query(field, term.image);
    }
    if (prefix) {
        // The trailing `*` is not part of the prefix.
        auto unescaped = discard_escape_char(std::string_view(term.image).substr(0, term.image.size() - 1));
        if (!unescaped.has_value()) {
            return std::unexpected(std::move(unescaped.error()));
        }
        return get_prefix_query(field, unescaped.value());
    }
    if (regexp) {
        return get_regexp_query(field, std::string_view(term.image).substr(1, term.image.size() - 2));
    }
    if (fuzzy) {
        if (fuzzy_slop == nullptr) {
            return err(LuceneError::illegal_state("a fuzzy term needs its `~n` suffix"));
        }
        const float min_similarity = get_fuzzy_distance(*fuzzy_slop);
        if (min_similarity < 0.0F) {
            return err(LuceneError::parse("Minimum similarity for a FuzzyQuery has to be between 0.0f and 1.0f !"));
        }
        if (min_similarity >= 1.0F && min_similarity != static_cast<float>(static_cast<int32_t>(min_similarity))) {
            return err(LuceneError::parse("Fractional edit distances are not allowed!"));
        }
        return get_fuzzy_query(field, term_image.value(), min_similarity);
    }
    return get_field_query(field, term_image.value(), false);
}

Result<std::shared_ptr<Query>> QueryParserBase::handle_quoted_term(std::string_view field, const QueryToken &term,
                                                                   const QueryToken *fuzzy_slop) {
    int32_t slop = phrase_slop_;
    if (fuzzy_slop != nullptr) {
        if (const auto parsed = parse_float(std::string_view(fuzzy_slop->image).substr(1)); parsed.has_value()) {
            slop = static_cast<int32_t>(parsed.value());
        }
    }
    auto text = discard_escape_char(std::string_view(term.image).substr(1, term.image.size() - 2));
    if (!text.has_value()) {
        return std::unexpected(std::move(text.error()));
    }
    return get_field_query_with_slop(field, text.value(), slop);
}

Result<std::shared_ptr<Query>> QueryParserBase::handle_boost(std::shared_ptr<Query> q, const QueryToken *boost) {
    if (boost == nullptr || q == nullptr) {
        return q;
    }
    float factor = 1.0F;
    if (const auto parsed = parse_float(boost->image); parsed.has_value()) {
        factor = parsed.value();
    }
    auto boosted = BoostQuery::create(std::move(q), factor);
    if (!boosted.has_value()) {
        return std::unexpected(std::move(boosted.error()));
    }
    return std::shared_ptr<Query>(std::make_shared<BoostQuery>(std::move(boosted.value())));
}

float QueryParserBase::get_fuzzy_distance(const QueryToken &fuzzy_token) const {
    if (const auto parsed = parse_float(std::string_view(fuzzy_token.image).substr(1)); parsed.has_value()) {
        return parsed.value();
    }
    return fuzzy_min_sim_;
}

// ---------------------------------------------------------------------------
// Normalization
// ---------------------------------------------------------------------------

Result<BytesRef> QueryParserBase::normalize(std::string_view field, std::string_view text) {
    auto stream_result = analyzer().token_stream(field, text);
    if (!stream_result.has_value()) {
        return std::unexpected(std::move(stream_result.error()));
    }
    TokenStream *stream = stream_result.value();
    if (VoidResult reset = stream->reset(); !reset.has_value()) {
        return std::unexpected(std::move(reset.error()));
    }

    AttributeSource &attributes = stream->attributes();
    auto *char_term = attributes.has_attribute<CharTermAttribute>() ? attributes.get_attribute<CharTermAttribute>()
                                                                   : nullptr;
    auto *bytes_term = attributes.has_attribute<BytesTermAttribute>()
                           ? attributes.get_attribute<BytesTermAttribute>()
                           : nullptr;

    std::vector<std::string> tokens;
    while (true) {
        auto advanced = stream->increment_token();
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        if (!advanced.value()) {
            break;
        }
        if (bytes_term != nullptr && !bytes_term->bytes_ref().empty()) {
            const std::span<const uint8_t> bytes = bytes_term->bytes_ref();
            tokens.emplace_back(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        } else if (char_term != nullptr) {
            tokens.emplace_back(char_term->text());
        }
    }
    if (VoidResult ended = stream->end(); !ended.has_value()) {
        return std::unexpected(std::move(ended.error()));
    }
    if (VoidResult closed = stream->close(); !closed.has_value()) {
        return std::unexpected(std::move(closed.error()));
    }

    // The reference implementation normalizes through a keyword tokenizer, so the
    // chain never splits the term; when this port's chain does split it, the text
    // is kept verbatim rather than picking an arbitrary token.
    if (tokens.size() == 1) {
        return BytesRef(std::vector<uint8_t>(tokens.front().begin(), tokens.front().end()));
    }
    if (tokens.empty()) {
        return BytesRef();
    }
    return BytesRef(std::vector<uint8_t>(text.begin(), text.end()));
}

Result<std::string> QueryParserBase::analyze_wildcard(std::string_view field, std::string_view term_str) {
    std::string out;
    out.reserve(term_str.size());

    const auto append_normalized = [&](std::string_view chunk) -> VoidResult {
        auto normalized = normalize(field, chunk);
        if (!normalized.has_value()) {
            return std::unexpected(std::move(normalized.error()));
        }
        const std::span<const uint8_t> bytes = normalized.value().as_slice();
        out.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        return {};
    };

    size_t last = 0;
    size_t i = 0;
    while (i < term_str.size()) {
        size_t match_begin = i;
        size_t match_end = i;

        if (term_str[i] == '\\' && i + 1 < term_str.size()) {
            // An escaped character belongs to the pattern as it is.
            match_end = i + 2;
        } else if (term_str[i] == '?' || term_str[i] == '*') {
            size_t j = i;
            while (j < term_str.size() && (term_str[j] == '?' || term_str[j] == '*')) {
                ++j;
            }
            match_end = j;
        } else {
            ++i;
            continue;
        }

        if (match_begin > 0) {
            if (VoidResult appended = append_normalized(term_str.substr(last, match_begin - last)); !appended.has_value()) {
                return std::unexpected(std::move(appended.error()));
            }
        }
        out.append(term_str.substr(match_begin, match_end - match_begin));
        last = match_end;
        i = match_end;
    }

    if (last < term_str.size()) {
        if (VoidResult appended = append_normalized(term_str.substr(last)); !appended.has_value()) {
            return std::unexpected(std::move(appended.error()));
        }
    }
    return out;
}

} // namespace lucene
