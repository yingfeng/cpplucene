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

module lucene_core:boolean_query.impl;

import :boolean_query;

import :boolean_clause;
import :boolean_weight;
import :error;
import :match_no_docs_query;
import :query;
import :query_visitor;
import :score_mode;
import :types;
import :weight;

import std;

namespace lucene {

namespace {

/// Process-wide clause limit, the reference implementation's
/// `IndexSearcher.maxClauseCount`.
std::atomic<size_t> g_max_clause_count{BooleanQuery::DEFAULT_MAX_CLAUSE_COUNT};

} // namespace

size_t BooleanQuery::max_clause_count() noexcept { return g_max_clause_count.load(std::memory_order_relaxed); }

VoidResult BooleanQuery::set_max_clause_count(size_t count) {
    if (count == 0) {
        return err(LuceneError::illegal_argument("maxClauseCount must be >= 1"));
    }
    g_max_clause_count.store(count, std::memory_order_relaxed);
    return {};
}

BooleanQuery::BooleanQuery(std::vector<BooleanClause> clauses, int32_t min_should_match)
    : clauses_(std::move(clauses)), min_should_match_(min_should_match) {
    for (size_t index = 0; index < clauses_.size(); ++index) {
        const Occur occur = clauses_[index].occur;
        std::vector<size_t> &indices = clause_sets_[static_cast<size_t>(occur)];
        if (occur == Occur::Should || occur == Occur::Must) {
            indices.push_back(index);
            continue;
        }
        // FILTER and MUST_NOT clauses are deduplicated: matching the same query
        // twice neither adds documents nor changes the score.
        const BooleanClause &clause = clauses_[index];
        bool exists = false;
        for (const size_t existing : indices) {
            if (clauses_[existing].query != nullptr && clause.query != nullptr &&
                clauses_[existing].query->equals(*clause.query)) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            indices.push_back(index);
        }
    }
}

Result<BooleanQuery> BooleanQuery::create(std::vector<BooleanClause> clauses, int32_t min_should_match) {
    if (min_should_match < 0) {
        return err(LuceneError::illegal_argument("minimum_number_should_match must be >= 0, got " +
                                                 std::to_string(min_should_match)));
    }
    if (clauses.size() > max_clause_count()) {
        return err(LuceneError::too_many_clauses("maxClauseCount is set to " +
                                                 std::to_string(max_clause_count())));
    }
    for (const BooleanClause &clause : clauses) {
        if (clause.query == nullptr) {
            return err(LuceneError::illegal_argument("BooleanQuery clause must not be null"));
        }
    }
    return BooleanQuery(std::move(clauses), min_should_match);
}

BooleanQuery::Builder &BooleanQuery::Builder::set_minimum_number_should_match(int32_t min) noexcept {
    min_should_match_ = min;
    return *this;
}

VoidResult BooleanQuery::Builder::add_clause(BooleanClause clause) {
    if (clauses_.size() >= max_clause_count()) {
        return err(LuceneError::too_many_clauses("maxClauseCount is set to " + std::to_string(max_clause_count())));
    }
    if (clause.query == nullptr) {
        return err(LuceneError::illegal_argument("BooleanQuery clause must not be null"));
    }
    clauses_.push_back(std::move(clause));
    return {};
}

VoidResult BooleanQuery::Builder::add(std::shared_ptr<Query> query, Occur occur) {
    return add_clause(BooleanClause::create(std::move(query), occur));
}

Result<BooleanQuery> BooleanQuery::Builder::build() {
    return BooleanQuery::create(std::move(clauses_), min_should_match_);
}

bool BooleanQuery::is_pure_disjunction() const noexcept {
    return clauses_.size() == clause_indices(Occur::Should).size() && min_should_match_ <= 1;
}

Result<std::unique_ptr<Weight>> BooleanQuery::create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                            float boost) const {
    auto weight = BooleanWeight::create(*this, searcher, score_mode, boost);
    if (!weight.has_value()) {
        return std::unexpected(std::move(weight.error()));
    }
    return std::unique_ptr<Weight>(std::move(weight.value()));
}

Result<std::shared_ptr<Query>> BooleanQuery::rewrite(const IndexSearcher &searcher) const {
    if (clauses_.empty()) {
        return std::shared_ptr<Query>(MatchNoDocsQuery::create("empty BooleanQuery"));
    }
    if (clauses_.size() == clause_indices(Occur::MustNot).size()) {
        // A pure negative query cannot match on its own: it needs something to
        // subtract from.
        return std::shared_ptr<Query>(MatchNoDocsQuery::create("pure negative BooleanQuery"));
    }

    // Optimise single-clause queries.
    if (clauses_.size() == 1) {
        const BooleanClause &clause = clauses_[0];
        if (min_should_match_ == 1 && clause.occur == Occur::Should) {
            return clause.query;
        }
        if (min_should_match_ == 0) {
            if (clause.occur == Occur::Should || clause.occur == Occur::Must) {
                return clause.query;
            }
        }
    }

    // Recursively rewrite the children.
    bool rewritten = false;
    std::vector<BooleanClause> new_clauses;
    new_clauses.reserve(clauses_.size());
    for (const BooleanClause &clause : clauses_) {
        auto child = clause.query->rewrite(searcher);
        if (!child.has_value()) {
            return std::unexpected(std::move(child.error()));
        }
        std::shared_ptr<Query> effective = clause.query;
        if (child.value() != nullptr) {
            effective = std::move(child.value());
            rewritten = true;
        }

        if (dynamic_cast<const MatchNoDocsQuery *>(effective.get()) != nullptr) {
            if (clause.occur == Occur::Should || clause.occur == Occur::MustNot) {
                // Cannot match: dropping the clause does not change the result.
                rewritten = true;
                continue;
            }
            // A required clause that matches nothing makes the query match
            // nothing.
            return std::shared_ptr<Query>(MatchNoDocsQuery::create("required clause matches no documents"));
        }

        if (new_clauses.size() >= max_clause_count()) {
            return err(LuceneError::too_many_clauses("maxClauseCount is set to " + std::to_string(max_clause_count())));
        }
        new_clauses.push_back(BooleanClause::create(std::move(effective), clause.occur));
    }

    if (!rewritten) {
        return std::shared_ptr<Query>{};
    }
    if (new_clauses.empty()) {
        return std::shared_ptr<Query>(MatchNoDocsQuery::create("empty BooleanQuery after rewrite"));
    }
    if (new_clauses.size() == 1) {
        const BooleanClause &only = new_clauses[0];
        if (only.occur == Occur::Should || only.occur == Occur::Must) {
            return only.query;
        }
    }
    auto query = BooleanQuery::create(std::move(new_clauses), min_should_match_);
    if (!query.has_value()) {
        return std::unexpected(std::move(query.error()));
    }
    return std::make_shared<BooleanQuery>(std::move(query.value()));
}

VoidResult BooleanQuery::visit(QueryVisitor &visitor) const {
    for (const BooleanClause &clause : clauses_) {
        if (auto visited = clause.query->visit(visitor); !visited.has_value()) {
            return visited;
        }
    }
    return {};
}

Result<std::string> BooleanQuery::to_string(std::string_view field) const {
    std::string buffer;
    const bool need_parens = min_should_match_ > 0;
    if (need_parens) {
        buffer.push_back('(');
    }

    for (size_t i = 0; i < clauses_.size(); ++i) {
        const BooleanClause &clause = clauses_[i];
        buffer.append(occur_prefix(clause.occur));
        auto text = clause.query->to_string(field);
        if (!text.has_value()) {
            return std::unexpected(std::move(text.error()));
        }
        const bool is_boolean = dynamic_cast<const BooleanQuery *>(clause.query.get()) != nullptr;
        if (is_boolean) {
            buffer.push_back('(');
            buffer.append(text.value());
            buffer.push_back(')');
        } else {
            buffer.append(text.value());
        }
        if (i + 1 != clauses_.size()) {
            buffer.push_back(' ');
        }
    }

    if (need_parens) {
        buffer.push_back(')');
    }
    if (min_should_match_ > 0) {
        buffer.push_back('~');
        buffer.append(std::to_string(min_should_match_));
    }
    return buffer;
}

bool BooleanQuery::equals(const Query &other) const {
    const auto *that = dynamic_cast<const BooleanQuery *>(&other);
    if (that == nullptr) {
        return false;
    }
    if (min_should_match_ != that->min_should_match_) {
        return false;
    }
    if (clauses_.size() != that->clauses_.size()) {
        return false;
    }
    for (size_t i = 0; i < clauses_.size(); ++i) {
        if (clauses_[i].occur != that->clauses_[i].occur) {
            return false;
        }
        if (!clauses_[i].query->equals(*that->clauses_[i].query)) {
            return false;
        }
    }
    return true;
}

size_t BooleanQuery::hash() const {
    size_t result = std::hash<int32_t>{}(min_should_match_);
    for (const BooleanClause &clause : clauses_) {
        // Order matters, like `equals`.
        result = (result * 31) + std::hash<int32_t>{}(static_cast<int32_t>(clause.occur));
        result = (result * 31) + clause.query->hash();
    }
    return result;
}

} // namespace lucene
