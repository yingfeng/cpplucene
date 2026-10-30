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

export module lucene_core:bulk_scorer;

import :types;
import std;

import :bits;
import :doc_id_set_iterator;
import :error;
import :leaf_collector;
import :scorable;
import :scorer;

namespace lucene {

/// Scores a range of documents at once.
///
/// Port of the `BulkScorer` trait (`core/search/bulk_scorer.rs`).
///
/// Queries that can score more efficiently than by iterating their scorer
/// provide their own implementation; everything else uses
/// [`BasicBulkScorer`].
export class BulkScorer {
public:
    virtual ~BulkScorer() = default;

    /// Collects the matches in `[min, max)` and returns an estimation of the
    /// next matching document at or after `max` ([`NO_MORE_DOCS`] when there is
    /// none).
    ///
    /// `accept_docs` restricts the documents that may match; `nullptr` accepts
    /// everything.
    [[nodiscard]] virtual Result<int32_t> score(LeafCollector &collector, const Bits *accept_docs, int32_t min,
                                                int32_t max) = 0;

    /// The same as [`DocIdSetIterator::cost`], for bulk scorers.
    [[nodiscard]] virtual Result<int64_t> cost() = 0;
};

/// The generic [`BulkScorer`]: iterates a scorer and feeds a collector.
///
/// Port of `DefaultBulkScorer` (`core/search/weight.rs`).
///
/// **Deviation:** this is a template over the concrete scorer type rather than a
/// class over `Scorer`.  The reference implementation monomorphises its bulk
/// scorers over the scorer, and so does this port: instantiating the loop where
/// the scorer type is known (`BasicBulkScorer<TermScorer>`) turns `next_doc()`
/// and `score()` into direct calls that inline into the loop, instead of one
/// virtual call per document each.  A caller that only has a `Scorer` still
/// instantiates the `Scorer` specialisation, which behaves exactly like the
/// type erased version.
export template <class ScorerT>
class BasicBulkScorer final : public BulkScorer {
public:
    explicit BasicBulkScorer(std::unique_ptr<ScorerT> scorer) : scorer_(std::move(scorer)) {}

    [[nodiscard]] Result<int32_t> score(LeafCollector &collector, const Bits *accept_docs, int32_t min,
                                        int32_t max) override {
        if (auto set = collector.set_scorer(*scorer_); !set.has_value()) {
            return std::unexpected(std::move(set.error()));
        }
        // Asked once per leaf: a collector that takes a pre-computed score lets
        // the loop score with the *static* scorer type, so `score()` inlines
        // instead of going through `Scorable::score` once per document.
        precomputed_scores_ = collector.supports_precomputed_score();
        if constexpr (requires { scorer_->has_fast_score(); }) {
            // The scorer resolved a branch-free scoring path: use it, and score
            // in this loop rather than through `Scorable::score`.
            fast_scoring_ = precomputed_scores_ && scorer_->has_fast_score();
        }
        const bool has_two_phase = scorer_->has_two_phase_iterator() == TwoPhaseState::Yes;
        const int32_t doc_id = scorer_->approx_doc_id();

        auto competitive = collector.competitive_iterator();
        if (!competitive.has_value()) {
            return std::unexpected(std::move(competitive.error()));
        }
        const bool has_competitive_iterator = competitive.value() != nullptr;

        if (!has_competitive_iterator && doc_id == -1 && accept_docs == nullptr && min == 0 && max == NO_MORE_DOCS) {
            if (auto scored = score_all(collector, accept_docs, has_two_phase); !scored.has_value()) {
                return std::unexpected(std::move(scored.error()));
            }
            return NO_MORE_DOCS;
        }
        return score_range(collector, accept_docs, min, max, has_competitive_iterator, has_two_phase);
    }

    [[nodiscard]] Result<int64_t> cost() override { return scorer_->cost(); }

private:
    /// Hands one hit to the collector, scoring it here when the collector takes a
    /// pre-computed score.
    [[nodiscard]] VoidResult hand_to_collector(LeafCollector &collector, int32_t doc) {
        if (!precomputed_scores_) {
            return collector.collect(doc, *scorer_);
        }
        // `scorer_` is the concrete type, so this call is bound statically and
        // inlines into the loop.
        Result<float> score = [this]() -> Result<float> {
            if constexpr (requires { scorer_->fast_score(); }) {
                if (fast_scoring_) {
                    return scorer_->fast_score();
                }
            }
            return scorer_->score();
        }();
        if (!score.has_value()) {
            return std::unexpected(std::move(score.error()));
        }
        auto taken = collector.collect_with_score(doc, *scorer_, score.value());
        if (!taken.has_value()) {
            return std::unexpected(std::move(taken.error()));
        }
        if (!taken.value()) {
            // The collector changed its mind; fall back to the scored call.
            return collector.collect(doc, *scorer_);
        }
        return {};
    }

    /// Whether `accept_docs` allows `doc`.
    [[nodiscard]] static Result<bool> is_accept_doc(const Bits *accept_docs, int32_t doc) {
        if (accept_docs == nullptr) {
            return true;
        }
        return accept_docs->get(static_cast<size_t>(doc));
    }

    /// Bulk-scores every hit.  Separate from [`score_range`] to keep the hot loop
    /// small (LUCENE-5487).
    [[nodiscard]] VoidResult score_all(LeafCollector &collector, const Bits *accept_docs, bool has_two_phase) {
        if (!has_two_phase) {
            for (;;) {
                auto doc = scorer_->next_doc();
                if (!doc.has_value()) {
                    return std::unexpected(std::move(doc.error()));
                }
                if (doc.value() == NO_MORE_DOCS) {
                    return {};
                }
                auto accept = is_accept_doc(accept_docs, doc.value());
                if (!accept.has_value()) {
                    return std::unexpected(std::move(accept.error()));
                }
                if (accept.value()) {
                    if (auto collected = hand_to_collector(collector, doc.value()); !collected.has_value()) {
                        return collected;
                    }
                }
            }
        }

        // The scorer has an approximation: run it first, then check acceptDocs,
        // then confirm.
        for (;;) {
            auto doc = scorer_->approx_next_doc();
            if (!doc.has_value()) {
                return std::unexpected(std::move(doc.error()));
            }
            if (doc.value() == NO_MORE_DOCS) {
                return {};
            }
            auto accept = is_accept_doc(accept_docs, doc.value());
            if (!accept.has_value()) {
                return std::unexpected(std::move(accept.error()));
            }
            if (accept.value()) {
                auto matched = scorer_->matches();
                if (!matched.has_value()) {
                    return std::unexpected(std::move(matched.error()));
                }
                if (matched.value()) {
                    if (auto collected = hand_to_collector(collector, doc.value()); !collected.has_value()) {
                        return collected;
                    }
                }
            }
        }
    }

    /// Bulk-scores the hits in `[min, max)`.  Separate from [`score_all`] for the
    /// same reason.
    [[nodiscard]] Result<int32_t> score_range(LeafCollector &collector, const Bits *accept_docs, int32_t min,
                                             int32_t max, bool has_competitive, bool has_two_phase) {
        if (has_competitive) {
            auto competitive = collector.competitive_iterator();
            if (!competitive.has_value()) {
                return std::unexpected(std::move(competitive.error()));
            }
            if (competitive.value() == nullptr) {
                return err(LuceneError::illegal_state("has_competitive is true but competitive_iterator is nullptr"));
            }
            if (competitive.value()->doc_id() > min) {
                // The competitive iterator may not match any docs in the range.
                min = std::min(competitive.value()->doc_id(), max);
            }
        }

        int32_t doc = scorer_->approx_doc_id();
        if (doc < min) {
            if (doc == min - 1) {
                auto next = scorer_->approx_next_doc();
                if (!next.has_value()) {
                    return std::unexpected(std::move(next.error()));
                }
                doc = next.value();
            } else {
                auto advanced = scorer_->approx_advance(min);
                if (!advanced.has_value()) {
                    return std::unexpected(std::move(advanced.error()));
                }
                doc = advanced.value();
            }
        }

        if (!has_two_phase && !has_competitive) {
            // Simple iterators with collectors that cannot skip.
            while (doc < max) {
                auto accept = is_accept_doc(accept_docs, doc);
                if (!accept.has_value()) {
                    return std::unexpected(std::move(accept.error()));
                }
                if (accept.value()) {
                    if (auto collected = hand_to_collector(collector, doc); !collected.has_value()) {
                        return std::unexpected(std::move(collected.error()));
                    }
                }
                auto next = scorer_->next_doc();
                if (!next.has_value()) {
                    return std::unexpected(std::move(next.error()));
                }
                doc = next.value();
            }
        } else {
            while (doc < max) {
                if (has_competitive) {
                    auto competitive = collector.competitive_iterator();
                    if (!competitive.has_value()) {
                        return std::unexpected(std::move(competitive.error()));
                    }
                    if (competitive.value() != nullptr) {
                        int32_t competitive_doc = competitive.value()->doc_id();
                        if (competitive_doc < doc) {
                            auto advanced = competitive.value()->advance(doc);
                            if (!advanced.has_value()) {
                                return std::unexpected(std::move(advanced.error()));
                            }
                            competitive_doc = advanced.value();
                        }
                        if (competitive_doc != doc) {
                            auto advanced = scorer_->approx_advance(competitive_doc);
                            if (!advanced.has_value()) {
                                return std::unexpected(std::move(advanced.error()));
                            }
                            doc = advanced.value();
                            continue;
                        }
                    }
                }

                auto accept = is_accept_doc(accept_docs, doc);
                if (!accept.has_value()) {
                    return std::unexpected(std::move(accept.error()));
                }
                bool matched = true;
                if (accept.value() && has_two_phase) {
                    auto matches = scorer_->matches();
                    if (!matches.has_value()) {
                        return std::unexpected(std::move(matches.error()));
                    }
                    matched = matches.value();
                }
                if (accept.value() && matched) {
                    if (auto collected = hand_to_collector(collector, doc); !collected.has_value()) {
                        return std::unexpected(std::move(collected.error()));
                    }
                }

                auto next = scorer_->approx_next_doc();
                if (!next.has_value()) {
                    return std::unexpected(std::move(next.error()));
                }
                doc = next.value();
            }
        }

        return doc;
    }

    std::unique_ptr<ScorerT> scorer_;
    /// Whether the collector of the current leaf takes a pre-computed score.
    bool precomputed_scores_{false};
    /// Whether the scorer's branch-free scoring path is used.
    bool fast_scoring_{false};
};

/// [`BasicBulkScorer`] over the type erased [`Scorer`].
export using DefaultBulkScorer = BasicBulkScorer<Scorer>;

} // namespace lucene
