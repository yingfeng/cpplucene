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

module lucene_core:boolean_weight.impl;

import :boolean_weight;

import :boolean_clause;
import :boolean_query;
import :boolean_scorer_supplier;
import :error;
import :explanation;
import :index_searcher;
import :leaf_reader;
import :leaf_reader_context;
import :query;
import :score_mode;
import :scorer;
import :scorer_supplier;
import :types;
import :weight;

import std;

namespace lucene {

namespace {

/// Above this many clauses a boolean query is not cached, so that nobody builds
/// huge boolean queries as a way around the term-in-set limit (the reference implementation's
/// `BOOLEAN_REWRITE_TERM_COUNT_THRESHOLD`).
constexpr size_t kCacheClauseLimit = 16;

constexpr size_t slot_of(Occur occur) noexcept { return static_cast<size_t>(occur); }

} // namespace

Result<std::unique_ptr<BooleanWeight>> BooleanWeight::create(const BooleanQuery &query, const IndexSearcher &searcher,
                                                             ScoreMode score_mode, float boost) {
    std::vector<std::unique_ptr<Weight>> sub_weights;
    sub_weights.reserve(query.clauses().size());
    for (const BooleanClause &clause : query.clauses()) {
        if (clause.query == nullptr) {
            return err(LuceneError::illegal_argument("BooleanQuery clause must not be null"));
        }
        // A clause that does not score never asks the searcher for scores.
        const ScoreMode clause_score_mode = clause.is_scoring() ? score_mode : ScoreMode::CompleteNoScores;
        auto sub_weight = searcher.create_weight(*clause.query, clause_score_mode, boost);
        if (!sub_weight.has_value()) {
            return std::unexpected(std::move(sub_weight.error()));
        }
        sub_weights.push_back(std::move(sub_weight.value()));
    }
    return std::unique_ptr<BooleanWeight>(new BooleanWeight(query, score_mode, std::move(sub_weights)));
}

const Query &BooleanWeight::get_query() const { return *query_; }

Result<Explanation> BooleanWeight::explain(const LeafReaderContext &context, int32_t doc,
                                           const IndexSearcher &searcher) const {
    const int32_t min_should_match = query_->minimum_number_should_match();

    std::vector<Explanation> subs;
    subs.reserve(sub_weights_.size());
    bool fail = false;
    int32_t match_count = 0;
    int32_t should_match_count = 0;

    for (size_t i = 0; i < sub_weights_.size(); ++i) {
        const BooleanClause &clause = query_->clauses()[i];
        auto explained = sub_weights_[i]->explain(context, doc, searcher);
        if (!explained.has_value()) {
            return std::unexpected(std::move(explained.error()));
        }
        Explanation explanation = std::move(explained.value());

        if (explanation.is_match()) {
            if (clause.is_scoring()) {
                subs.push_back(std::move(explanation));
            } else if (clause.is_required()) {
                std::vector<Explanation> details;
                details.push_back(Explanation::match_no_details(0.0F, std::string(occur_prefix(Occur::Filter)) + " clause"));
                details.push_back(std::move(explanation));
                subs.push_back(Explanation::match_(0.0F, "match on required clause, product of:", std::move(details)));
            } else if (clause.is_prohibited()) {
                auto query_text = clause.query->to_string("");
                std::string description = "match on prohibited clause (";
                description += query_text.has_value() ? query_text.value() : std::string("<query>");
                description += ")";
                std::vector<Explanation> details;
                details.push_back(std::move(explanation));
                subs.push_back(Explanation::no_match(std::move(description), std::move(details)));
                fail = true;
            }
            if (!clause.is_prohibited()) {
                ++match_count;
            }
            if (clause.occur == Occur::Should) {
                ++should_match_count;
            }
        } else if (clause.is_required()) {
            auto query_text = clause.query->to_string("");
            std::string description = "no match on required clause (";
            description += query_text.has_value() ? query_text.value() : std::string("<query>");
            description += ")";
            std::vector<Explanation> details;
            details.push_back(std::move(explanation));
            subs.push_back(Explanation::no_match(std::move(description), std::move(details)));
            fail = true;
        }
    }

    if (fail) {
        return Explanation::no_match("Failure to meet condition(s) of required/prohibited clause(s)", std::move(subs));
    }
    if (match_count == 0) {
        return Explanation::no_match("No matching clauses", std::move(subs));
    }
    if (should_match_count < min_should_match) {
        return Explanation::no_match(
                "Failure to match minimum number of optional clauses: " + std::to_string(min_should_match),
                std::move(subs));
    }

    // Pull a scorer and use it for the value, so that the explanation reports
    // exactly what the scorer would score (float rounding included).
    //
    // Building a scorer mutates the weight in the reference implementation too (interior mutability),
    // and `explain` cannot be non-const because `Weight::explain` is const.
    auto scorer = const_cast<BooleanWeight *>(this)->Weight::scorer(context, searcher);
    if (!scorer.has_value()) {
        return std::unexpected(std::move(scorer.error()));
    }
    if (scorer.value() == nullptr) {
        return err(LuceneError::illegal_state("no scorer available for explanation"));
    }
    auto advanced = scorer.value()->advance(doc);
    if (!advanced.has_value()) {
        return std::unexpected(std::move(advanced.error()));
    }
    auto score = scorer.value()->score();
    if (!score.has_value()) {
        return std::unexpected(std::move(score.error()));
    }
    return Explanation::match_(score.value(), "sum of:", std::move(subs));
}

Result<std::unique_ptr<ScorerSupplier>> BooleanWeight::scorer_supplier(const LeafReaderContext &context,
                                                                       const IndexSearcher &searcher) {
    int32_t min_should_match = query_->minimum_number_should_match();

    std::array<std::vector<std::unique_ptr<ScorerSupplier>>, 4> subs;
    for (size_t i = 0; i < sub_weights_.size(); ++i) {
        const BooleanClause &clause = query_->clauses()[i];
        auto supplier = sub_weights_[i]->scorer_supplier(context, searcher);
        if (!supplier.has_value()) {
            return std::unexpected(std::move(supplier.error()));
        }
        if (supplier.value() == nullptr) {
            // A required clause that cannot match makes the whole query
            // unmatched; a missing optional clause is simply dropped.
            if (clause.is_required()) {
                return std::unique_ptr<ScorerSupplier>{};
            }
            continue;
        }
        subs[slot_of(clause.occur)].push_back(std::move(supplier.value()));
    }

    // If the number of optional clauses equals the minimum, they are all
    // required.
    if (static_cast<int32_t>(subs[slot_of(Occur::Should)].size()) == min_should_match) {
        auto &should_subs = subs[slot_of(Occur::Should)];
        auto &must_subs = subs[slot_of(Occur::Must)];
        if (must_subs.empty()) {
            must_subs = std::move(should_subs);
        } else {
            // Draining the source list matters: the reference implementation uses
            // `must.append(&mut should)`, which leaves `should` empty.  Keeping
            // the (now null) slots behind would make `optional()` dereference
            // null suppliers.
            must_subs.insert(must_subs.end(), std::make_move_iterator(should_subs.begin()),
                             std::make_move_iterator(should_subs.end()));
            should_subs.clear();
        }
        min_should_match = 0;
    }

    if ((subs[slot_of(Occur::Filter)].empty() && subs[slot_of(Occur::Must)].empty() &&
         subs[slot_of(Occur::Should)].empty()) ||
        static_cast<int32_t>(subs[slot_of(Occur::Should)].size()) < min_should_match) {
        return std::unique_ptr<ScorerSupplier>{};
    }

    // Without scores the optional clauses cannot affect the result, as long as
    // there is something required to drive the iteration.
    if (!score_mode_needs_scores(score_mode_) && min_should_match == 0 &&
        (subs[slot_of(Occur::Must)].size() + subs[slot_of(Occur::Filter)].size()) > 0) {
        subs[slot_of(Occur::Should)].clear();
    }

    auto max_doc = context.reader()->max_doc();
    if (!max_doc.has_value()) {
        return std::unexpected(std::move(max_doc.error()));
    }
    auto supplier = BooleanScorerSupplier::create(std::move(subs), score_mode_, min_should_match, max_doc.value());
    if (!supplier.has_value()) {
        return std::unexpected(std::move(supplier.error()));
    }
    return std::unique_ptr<ScorerSupplier>(std::move(supplier.value()));
}

Result<int32_t> BooleanWeight::req_count(const LeafReaderContext &context, const IndexSearcher &searcher) {
    auto num_docs_result = context.reader()->num_docs();
    if (!num_docs_result.has_value()) {
        return std::unexpected(std::move(num_docs_result.error()));
    }
    const int32_t num_docs = num_docs_result.value();
    int32_t req_count = num_docs;

    for (size_t i = 0; i < sub_weights_.size(); ++i) {
        if (!query_->clauses()[i].is_required()) {
            continue;
        }
        auto count = sub_weights_[i]->count(context, searcher);
        if (!count.has_value()) {
            return std::unexpected(std::move(count.error()));
        }
        if (count.value() == -1 || count.value() == 0) {
            // Unknown stays unknown; zero means the conjunction matches nothing.
            return count.value();
        }
        if (count.value() == num_docs) {
            // Matches everything: it cannot narrow the count down.
        } else if (req_count == num_docs) {
            // Every clause seen so far matched everything, so this is the count.
            req_count = count.value();
        } else {
            // Two clauses with a count in (0, num_docs) cannot be combined
            // without running the query.
            return -1;
        }
    }
    return req_count;
}

Result<int32_t> BooleanWeight::opt_count(const LeafReaderContext &context, Occur occur,
                                         const IndexSearcher &searcher) {
    auto num_docs_result = context.reader()->num_docs();
    if (!num_docs_result.has_value()) {
        return std::unexpected(std::move(num_docs_result.error()));
    }
    const int32_t num_docs = num_docs_result.value();
    int32_t opt_count = 0;
    bool unknown_count = false;

    for (size_t i = 0; i < sub_weights_.size(); ++i) {
        if (query_->clauses()[i].occur != occur) {
            continue;
        }
        auto count = sub_weights_[i]->count(context, searcher);
        if (!count.has_value()) {
            return std::unexpected(std::move(count.error()));
        }
        if (count.value() == -1) {
            unknown_count = true;
            continue;
        }
        if (count.value() == num_docs) {
            // A clause that matches everything makes the disjunction match
            // everything.
            return count.value();
        }
        if (count.value() == 0) {
            continue;
        }
        if (opt_count == 0) {
            opt_count = count.value();
        } else {
            unknown_count = true;
        }
    }
    return unknown_count ? -1 : opt_count;
}

Result<int32_t> BooleanWeight::count(const LeafReaderContext &context, const IndexSearcher &searcher) {
    auto num_docs_result = context.reader()->num_docs();
    if (!num_docs_result.has_value()) {
        return std::unexpected(std::move(num_docs_result.error()));
    }
    const int32_t num_docs = num_docs_result.value();

    if (query_->is_pure_disjunction()) {
        return opt_count(context, Occur::Should, searcher);
    }

    int32_t positive_count = -1;
    if ((!query_->clause_indices(Occur::Filter).empty() || !query_->clause_indices(Occur::Must).empty()) &&
        query_->minimum_number_should_match() == 0) {
        auto required = req_count(context, searcher);
        if (!required.has_value()) {
            return std::unexpected(std::move(required.error()));
        }
        positive_count = required.value();
    }
    if (positive_count == 0) {
        return 0;
    }

    auto prohibited = opt_count(context, Occur::MustNot, searcher);
    if (!prohibited.has_value()) {
        return std::unexpected(std::move(prohibited.error()));
    }
    const int32_t prohibited_count = prohibited.value();
    if (prohibited_count == -1) {
        return -1;
    }
    if (prohibited_count == 0) {
        return positive_count;
    }
    if (prohibited_count == num_docs) {
        return 0;
    }
    if (positive_count == num_docs) {
        return num_docs - prohibited_count;
    }
    return -1;
}

Result<bool> BooleanWeight::is_cacheable(const LeafReaderContext &context) const {
    if (query_->clauses().size() > kCacheClauseLimit) {
        return false;
    }
    for (const auto &weight : sub_weights_) {
        auto cacheable = weight->is_cacheable(context);
        if (!cacheable.has_value()) {
            return std::unexpected(std::move(cacheable.error()));
        }
        if (!cacheable.value()) {
            return false;
        }
    }
    return true;
}

} // namespace lucene
