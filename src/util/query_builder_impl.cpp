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

module lucene_core:query_builder.impl;

import :query_builder;

import :analyzer;
import :attributes;
import :boolean_clause;
import :boolean_query;
import :boost_query;
import :bytes_ref;
import :error;
import :multi_phrase_query;
import :occur;
import :phrase_query;
import :query;
import :synonym_query;
import :term;
import :term_query;
import :token_stream;
import :types;

import std;

namespace lucene {

namespace {

/// One token as the analysis chain produced it.
struct AnalyzedToken {
    std::string text;
    uint32_t pos_inc{1};
    uint32_t pos_len{1};
};

Term make_term(std::string_view field, const std::string &text) {
    // The token bytes are already UTF-8, so they are copied as-is instead of
    // going through `Term::from_text` (which would re-encode them).
    return Term(std::string(field), BytesRef(std::vector<uint8_t>(text.begin(), text.end())));
}

/// Runs the analyzer over `text` and collects every token, together with the
/// position increment and position length it carried.
///
/// **Deviation:** the reference implementation walks the (cached) token stream
/// twice — once to classify it, once to build the query.  This port collects the
/// tokens into a vector during a single walk; the classification then looks at
/// the vector.  That avoids porting `CachingTokenFilter` for no behavioural
/// difference, at the cost of holding one string per token.
Result<std::vector<AnalyzedToken>> analyze_tokens(Analyzer &analyzer, std::string_view field, std::string_view text) {
    auto stream_result = analyzer.token_stream(field, text);
    if (!stream_result.has_value()) {
        return std::unexpected(std::move(stream_result.error()));
    }
    TokenStream *stream = stream_result.value();
    if (VoidResult reset = stream->reset(); !reset.has_value()) {
        return std::unexpected(std::move(reset.error()));
    }

    AttributeSource &attributes = stream->attributes();
    const bool has_char_term = attributes.has_attribute<CharTermAttribute>();
    const bool has_bytes_term = attributes.has_attribute<BytesTermAttribute>();
    auto *char_term = has_char_term ? attributes.get_attribute<CharTermAttribute>() : nullptr;
    auto *bytes_term = has_bytes_term ? attributes.get_attribute<BytesTermAttribute>() : nullptr;
    auto *pos_inc = attributes.has_attribute<PositionIncrementAttribute>()
                        ? attributes.get_attribute<PositionIncrementAttribute>()
                        : nullptr;
    auto *pos_len = attributes.has_attribute<PositionLengthAttribute>()
                        ? attributes.get_attribute<PositionLengthAttribute>()
                        : nullptr;

    std::vector<AnalyzedToken> tokens;
    while (true) {
        auto advanced = stream->increment_token();
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        if (!advanced.value()) {
            break;
        }

        AnalyzedToken token;
        if (bytes_term != nullptr && !bytes_term->bytes_ref().empty()) {
            const std::span<const uint8_t> bytes = bytes_term->bytes_ref();
            token.text.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        } else if (char_term != nullptr) {
            token.text = std::string(char_term->text());
        }
        token.pos_inc = pos_inc != nullptr ? pos_inc->position_increment() : 1;
        token.pos_len = pos_len != nullptr ? pos_len->position_length() : 1;
        tokens.push_back(std::move(token));
    }

    if (VoidResult ended = stream->end(); !ended.has_value()) {
        return std::unexpected(std::move(ended.error()));
    }
    if (VoidResult closed = stream->close(); !closed.has_value()) {
        return std::unexpected(std::move(closed.error()));
    }
    return tokens;
}

} // namespace

QueryBuilder::QueryBuilder(Analyzer &analyzer) : analyzer_(&analyzer) {}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

Result<std::shared_ptr<Query>> QueryBuilder::create_boolean_query(std::string_view field, std::string_view query_text) {
    return create_boolean_query(field, query_text, Occur::Should);
}

Result<std::shared_ptr<Query>> QueryBuilder::create_boolean_query(std::string_view field, std::string_view query_text,
                                                                 Occur occur) {
    if (occur != Occur::Should && occur != Occur::Must) {
        return err(LuceneError::illegal_argument("invalid operator: only Should and Must are allowed"));
    }
    return create_field_query(occur, field, query_text, false, 0);
}

Result<std::shared_ptr<Query>> QueryBuilder::create_phrase_query(std::string_view field, std::string_view query_text,
                                                                 int32_t phrase_slop) {
    return create_field_query(Occur::Must, field, query_text, true, phrase_slop);
}

Result<std::shared_ptr<Query>> QueryBuilder::create_field_query(Occur occur, std::string_view field,
                                                                std::string_view query_text, bool quoted,
                                                                int32_t phrase_slop) {
    if (occur != Occur::Should && occur != Occur::Must) {
        return err(LuceneError::illegal_argument("invalid operator: only Should and Must are allowed"));
    }

    auto analyzed = analyze_tokens(*analyzer_, field, query_text);
    if (!analyzed.has_value()) {
        return std::unexpected(std::move(analyzed.error()));
    }
    const std::vector<AnalyzedToken> &tokens = analyzed.value();
    if (tokens.empty()) {
        // Nothing survived the analysis chain (an empty query, or stop words
        // only): there is no query to build.
        return std::shared_ptr<Query>{};
    }

    int32_t position_count = 0;
    bool has_synonyms = false;
    for (const AnalyzedToken &token : tokens) {
        if (token.pos_inc != 0) {
            position_count += static_cast<int32_t>(token.pos_inc);
        } else {
            has_synonyms = true;
        }
    }
    // A token with `position_length > 1` opens a graph (a multi-word synonym).
    // Graph queries need `GraphTokenStreamFiniteStrings`, which is not ported
    // yet, so such a stream is treated as if every token spanned one position.

    if (tokens.size() == 1) {
        const AnalyzedToken &token = tokens.front();
        return new_term_query(make_term(field, token.text), DEFAULT_BOOST);
    }

    if (quoted && position_count > 1) {
        // Several positions: a phrase, or a multi-phrase when a position carries
        // more than one term.
        if (has_synonyms) {
            std::vector<std::vector<Term>> terms_at_positions;
            std::vector<int32_t> positions;
            std::vector<Term> current;
            int32_t position = -1;
            for (const AnalyzedToken &token : tokens) {
                if (token.pos_inc > 0 && !current.empty()) {
                    terms_at_positions.push_back(std::move(current));
                    positions.push_back(position);
                    current.clear();
                }
                position += enable_position_increments_ ? static_cast<int32_t>(token.pos_inc) : 1;
                current.push_back(make_term(field, token.text));
            }
            if (!current.empty()) {
                terms_at_positions.push_back(std::move(current));
                positions.push_back(position);
            }
            return new_multi_phrase_query(std::move(terms_at_positions), std::move(positions), phrase_slop);
        }

        std::vector<Term> terms;
        std::vector<int32_t> positions;
        terms.reserve(tokens.size());
        positions.reserve(tokens.size());
        int32_t position = -1;
        for (const AnalyzedToken &token : tokens) {
            position += enable_position_increments_ ? static_cast<int32_t>(token.pos_inc) : 1;
            terms.push_back(make_term(field, token.text));
            positions.push_back(position);
        }
        return new_phrase_query(std::string(field), std::move(terms), std::move(positions), phrase_slop);
    }

    if (position_count == 1) {
        // Every token sits at the same position: pure synonyms.
        std::vector<TermAndBoost> synonyms;
        synonyms.reserve(tokens.size());
        for (const AnalyzedToken &token : tokens) {
            synonyms.push_back(TermAndBoost{token.text, DEFAULT_BOOST});
        }
        return new_synonym_query(field, synonyms);
    }

    // Several positions: one clause per position, joined with `occur`.
    std::vector<BooleanClause> clauses;
    std::vector<TermAndBoost> current;
    const auto flush_group = [&]() -> VoidResult {
        if (current.empty()) {
            return {};
        }
        Result<std::shared_ptr<Query>> built = current.size() == 1
                                                   ? new_term_query(make_term(field, current.front().text),
                                                                    current.front().boost)
                                                   : new_synonym_query(field, current);
        if (!built.has_value()) {
            return std::unexpected(std::move(built.error()));
        }
        if (built.value() != nullptr) {
            clauses.push_back(BooleanClause::create(std::move(built.value()), occur));
        }
        current.clear();
        return {};
    };

    for (const AnalyzedToken &token : tokens) {
        if (token.pos_inc != 0) {
            if (VoidResult flushed = flush_group(); !flushed.has_value()) {
                return std::unexpected(std::move(flushed.error()));
            }
        }
        current.push_back(TermAndBoost{token.text, DEFAULT_BOOST});
    }
    if (VoidResult flushed = flush_group(); !flushed.has_value()) {
        return std::unexpected(std::move(flushed.error()));
    }
    return new_boolean_query(std::move(clauses));
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

Result<std::shared_ptr<Query>> QueryBuilder::new_term_query(const Term &term, float boost) {
    std::shared_ptr<Query> query = std::make_shared<TermQuery>(term);
    if (boost == DEFAULT_BOOST) {
        return query;
    }
    auto boosted = BoostQuery::create(std::move(query), boost);
    if (!boosted.has_value()) {
        return std::unexpected(std::move(boosted.error()));
    }
    return std::shared_ptr<Query>(std::make_shared<BoostQuery>(std::move(boosted.value())));
}

Result<std::shared_ptr<Query>> QueryBuilder::new_synonym_query(std::string_view field,
                                                               std::span<const TermAndBoost> terms) {
    if (terms.empty()) {
        return std::shared_ptr<Query>{};
    }
    if (terms.size() == 1) {
        return new_term_query(make_term(field, terms.front().text), terms.front().boost);
    }

    SynonymQuery::Builder builder;
    for (const TermAndBoost &term : terms) {
        if (VoidResult added = builder.add_term(make_term(field, term.text), term.boost); !added.has_value()) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return std::shared_ptr<Query>(std::make_shared<SynonymQuery>(builder.build()));
}

Result<std::shared_ptr<Query>> QueryBuilder::new_boolean_query(std::vector<BooleanClause> clauses) {
    auto built = BooleanQuery::create(std::move(clauses));
    if (!built.has_value()) {
        return std::unexpected(std::move(built.error()));
    }
    return std::shared_ptr<Query>(std::make_shared<BooleanQuery>(std::move(built.value())));
}

Result<std::shared_ptr<Query>> QueryBuilder::new_phrase_query(std::string field, std::vector<Term> terms,
                                                              std::vector<int32_t> positions, int32_t slop) {
    auto built = PhraseQuery::with_positions(std::move(field), std::move(terms), std::move(positions), slop);
    if (!built.has_value()) {
        return std::unexpected(std::move(built.error()));
    }
    return std::shared_ptr<Query>(std::make_shared<PhraseQuery>(std::move(built.value())));
}

Result<std::shared_ptr<Query>> QueryBuilder::new_multi_phrase_query(std::vector<std::vector<Term>> terms_at_positions,
                                                                   std::vector<int32_t> positions, int32_t slop) {
    MultiPhraseQuery::Builder builder;
    if (VoidResult set = builder.set_slop(slop); !set.has_value()) {
        return std::unexpected(std::move(set.error()));
    }
    for (size_t i = 0; i < terms_at_positions.size(); ++i) {
        if (VoidResult added = builder.add_terms_with_position(std::move(terms_at_positions[i]), positions[i]);
            !added.has_value()) {
            return std::unexpected(std::move(added.error()));
        }
    }
    auto built = builder.build();
    if (!built.has_value()) {
        return std::unexpected(std::move(built.error()));
    }
    return std::shared_ptr<Query>(std::make_shared<MultiPhraseQuery>(std::move(built.value())));
}

} // namespace lucene
