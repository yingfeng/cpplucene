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

module lucene_core:n_gram_phrase_query.impl;

import :n_gram_phrase_query;

import std;

import :error;
import :index_searcher;
import :phrase_query;
import :query;
import :query_visitor;
import :score_mode;
import :term;
import :types;
import :weight;

namespace lucene {

NGramPhraseQuery::NGramPhraseQuery(size_t n, PhraseQuery phrase_query)
        : n_(n), phrase_query_(std::move(phrase_query)) {}

Result<std::unique_ptr<Weight>> NGramPhraseQuery::create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const {
    return phrase_query_.create_weight(searcher, score_mode, boost);
}

Result<std::shared_ptr<Query>> NGramPhraseQuery::rewrite(const IndexSearcher &searcher) const {
    const std::vector<Term> &terms = phrase_query_.terms();
    const std::vector<int32_t> &positions = phrase_query_.positions();

    bool consecutive = true;
    for (size_t i = 1; i < positions.size(); ++i) {
        if (positions[i] != positions[i - 1] + 1) {
            consecutive = false;
            break;
        }
    }
    const bool optimizable = phrase_query_.slop() == 0 && n_ >= 2 && terms.size() >= 3 && consecutive;

    if (!optimizable) {
        return phrase_query_.rewrite(searcher);
    }

    std::vector<Term> kept_terms;
    std::vector<int32_t> kept_positions;
    for (size_t i = 0; i < terms.size(); ++i) {
        if (i % n_ == 0 || i == terms.size() - 1) {
            kept_terms.push_back(terms[i]);
            kept_positions.push_back(positions[i]);
        }
    }
    if (kept_terms.size() == terms.size()) {
        // Nothing to drop, so the wrapped query is already optimal.
        return std::shared_ptr<Query>{};
    }
    auto optimized = PhraseQuery::with_positions(phrase_query_.field(), std::move(kept_terms),
                                                std::move(kept_positions), phrase_query_.slop());
    if (!optimized.has_value()) {
        return std::unexpected(std::move(optimized.error()));
    }
    return std::shared_ptr<Query>(std::make_shared<PhraseQuery>(std::move(optimized.value())));
}

VoidResult NGramPhraseQuery::visit(QueryVisitor &visitor) const { return phrase_query_.visit(visitor); }

Result<std::string> NGramPhraseQuery::to_string(std::string_view field) const {
    return phrase_query_.to_string(field);
}

bool NGramPhraseQuery::equals(const Query &other) const {
    const auto *n_gram = dynamic_cast<const NGramPhraseQuery *>(&other);
    if (n_gram == nullptr) {
        return false;
    }
    return n_ == n_gram->n_ && phrase_query_.equals(n_gram->phrase_query_);
}

size_t NGramPhraseQuery::hash() const { return phrase_query_.hash() * 31 + n_; }

} // namespace lucene
