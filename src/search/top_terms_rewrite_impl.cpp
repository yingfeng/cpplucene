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

module lucene_core:top_terms_rewrite.impl;

import :top_terms_rewrite;

import std;

import :attribute_source;
import :blended_term_query;
import :boolean_clause;
import :boolean_query;
import :boost_query;
import :bytes_ref;
import :constant_score_query;
import :error;
import :index_searcher;
import :index_reader;
import :leaf_reader_context;
import :match_no_docs_query;
import :max_non_competitive_boost_attribute;
import :multi_term_query;
import :priority_queue;
import :query;
import :term;
import :term_collecting_rewrite;
import :term_query;
import :term_states;
import :terms_enum;
import :types;

namespace lucene {

namespace {

/// One entry of the score-term queue: the term and the boost it was collected
/// with.
struct ScoreTermEntry {
    BytesRef key{};
    float boost{0.0F};
};

/// Orders the queue so that its top is the entry that has to be dropped first:
/// the lowest boost and, among equal boosts, the largest term.
struct ScoreTermEntryCompare {
    [[nodiscard]] Result<bool> less_than(const ScoreTermEntry &left, const ScoreTermEntry &right) const {
        if (left.boost == right.boost) {
            return right.key.compare_to(left.key) < 0;
        }
        return left.boost < right.boost;
    }
};

/// A collected term: its boost plus the states of every leaf it occurs in.
struct ScoreTerm {
    float boost{0.0F};
    TermStates states;

    ScoreTerm(float boost, TermStates states) : boost(boost), states(std::move(states)) {}
};

/// Keeps the `max_size` best-boosted terms.
///
/// Port of `TermCollectorImpl` (`core/search/top_terms_rewrite.rs`).
class TopTermsCollector final : public TermCollector {
public:
    TopTermsCollector(size_t max_size, const IndexReader &reader) : max_size_(max_size), reader_(&reader) {
        auto queue = PriorityQueue<ScoreTermEntry, ScoreTermEntryCompare>::create(max_size + 1, ScoreTermEntryCompare{});
        if (queue.has_value()) {
            queue_.emplace(std::move(queue.value()));
        } else {
            queue_error_ = std::make_unique<LuceneError>(std::move(queue.error()));
        }
    }

    [[nodiscard]] VoidResult set_reader_context(const LeafReaderContext &context) override {
        ord_ = context.ord();
        return queue_error_ == nullptr ? VoidResult{} : std::unexpected(*queue_error_);
    }

    [[nodiscard]] Result<bool> collect(const BytesRef &bytes, TermsEnum &terms_enum,
                                       const IndexSearcher &searcher) override {
        (void)searcher;
        if (queue_error_ != nullptr) {
            return std::unexpected(*queue_error_);
        }
        auto boost = terms_enum_boost(terms_enum);
        if (!boost.has_value()) {
            return std::unexpected(std::move(boost.error()));
        }

        if (queue_.value().size() == max_size_ && max_size_ > 0) {
            const ScoreTermEntry *worst = queue_.value().top();
            const auto current = visited_.find(worst->key);
            if (current != visited_.end()) {
                if (boost.value() < current->second.boost) {
                    return true;
                }
                if (boost.value() == current->second.boost && bytes.compare_to(worst->key) > 0) {
                    return true;
                }
            }
        }

        auto state = terms_enum.term_state();
        if (!state.has_value()) {
            return std::unexpected(std::move(state.error()));
        }
        auto doc_freq = terms_enum.doc_freq();
        if (!doc_freq.has_value()) {
            return std::unexpected(std::move(doc_freq.error()));
        }
        auto total_term_freq = terms_enum.total_term_freq();
        if (!total_term_freq.has_value()) {
            return std::unexpected(std::move(total_term_freq.error()));
        }

        const auto existing = visited_.find(bytes);
        if (existing != visited_.end()) {
            existing->second.states.register_with_stats(std::move(state.value()), ord_, doc_freq.value(),
                                                        total_term_freq.value());
            return true;
        }

        auto states = TermStates::create(*reader_);
        if (!states.has_value()) {
            return std::unexpected(std::move(states.error()));
        }
        ScoreTerm score_term(boost.value(), std::move(states.value()));
        score_term.states.register_with_stats(std::move(state.value()), ord_, doc_freq.value(),
                                              total_term_freq.value());

        visited_.emplace(bytes, std::move(score_term));
        if (auto added = queue_.value().add(ScoreTermEntry{bytes, boost.value()}); !added.has_value()) {
            visited_.erase(bytes);
            return std::unexpected(std::move(added.error()));
        }

        if (queue_.value().size() > max_size_) {
            auto dropped = queue_.value().pop();
            if (!dropped.has_value()) {
                return std::unexpected(std::move(dropped.error()));
            }
            if (dropped.value().has_value()) {
                visited_.erase(dropped.value().value().key);
            }
        }

        // Tell the enumeration which boost is no longer worth collecting, so it
        // can stop early.
        if (queue_.value().size() == max_size_ && max_size_ > 0) {
            const ScoreTermEntry *worst = queue_.value().top();
            const auto current = visited_.find(worst->key);
            if (current != visited_.end()) {
                auto attributes = terms_enum.attributes_mut();
                if (attributes.has_value() && attributes.value() != nullptr) {
                    if (auto *attribute = attributes.value()->get_attribute<MaxNonCompetitiveBoostAttribute>();
                        attribute != nullptr) {
                        attribute->set_max_non_competitive_boost(current->second.boost);
                        attribute->set_competitive_term(worst->key);
                    }
                }
            }
        }
        return true;
    }

    [[nodiscard]] VoidResult set_next_enum(TermsEnum &terms_enum) override {
        (void)terms_enum;
        return {};
    }

    /// The retained terms, in ascending term order; the map keeps them sorted.
    [[nodiscard]] const std::map<BytesRef, ScoreTerm> &visited_terms() const noexcept { return visited_; }

    /// Mutable access, so the term states can be moved into the query that is
    /// being built (they are move-only).
    [[nodiscard]] std::map<BytesRef, ScoreTerm> &visited_terms_mut() noexcept { return visited_; }

private:
    size_t max_size_;
    const IndexReader *reader_;
    std::optional<PriorityQueue<ScoreTermEntry, ScoreTermEntryCompare>> queue_{};
    std::unique_ptr<LuceneError> queue_error_{};
    std::map<BytesRef, ScoreTerm> visited_{};
    int32_t ord_{0};
};

} // namespace

Result<std::shared_ptr<Query>> rewrite_top_terms(const IndexSearcher &searcher, const MultiTermQuery &query,
                                                 size_t size, TopTermsBuildMode mode) {
    const size_t max_size = std::min(size, BooleanQuery::max_clause_count());
    TopTermsCollector collector(max_size, searcher.reader());
    if (auto collected = collect_terms(searcher, query, collector); !collected.has_value()) {
        return std::unexpected(std::move(collected.error()));
    }
    auto &visited = collector.visited_terms_mut();
    if (visited.empty()) {
        return std::shared_ptr<Query>(new MatchNoDocsQuery());
    }

    if (mode == TopTermsBuildMode::kBlendedFreq) {
        BlendedTermQuery::Builder builder;
        for (auto &[key, score_term] : visited) {
            auto states = std::make_shared<TermStates>(std::move(score_term.states));
            if (auto added = builder.add_with_term_states(Term(query.field(), key),
                                                          std::max(0.0F, score_term.boost), std::move(states));
                !added.has_value()) {
                return std::unexpected(std::move(added.error()));
            }
        }
        auto built = builder.build();
        if (!built.has_value()) {
            return std::unexpected(std::move(built.error()));
        }
        return std::shared_ptr<Query>(new BlendedTermQuery(std::move(built.value())));
    }

    BooleanQuery::Builder builder;
    for (auto &[key, score_term] : visited) {
        auto states = std::make_shared<TermStates>(std::move(score_term.states));
        std::shared_ptr<Query> term_query = std::make_shared<TermQuery>(Term(query.field(), key), std::move(states));
        if (mode == TopTermsBuildMode::kBoostOnlyBoolean) {
            term_query = std::make_shared<ConstantScoreQuery>(std::move(term_query));
        }
        const float boost = std::max(0.0F, score_term.boost);
        if (boost != 1.0F) {
            auto boosted = BoostQuery::create(term_query, boost);
            if (!boosted.has_value()) {
                return std::unexpected(std::move(boosted.error()));
            }
            term_query = std::make_shared<BoostQuery>(std::move(boosted.value()));
        }
        if (auto added = builder.add(std::move(term_query), Occur::Should); !added.has_value()) {
            return std::unexpected(std::move(added.error()));
        }
    }
    auto built = builder.build();
    if (!built.has_value()) {
        return std::unexpected(std::move(built.error()));
    }
    return std::shared_ptr<Query>(new BooleanQuery(std::move(built.value())));
}

} // namespace lucene
