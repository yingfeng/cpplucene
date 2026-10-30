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
///
/// The loop is also specialised at compile time over the two decisions that are
/// loop invariant in the reference implementation but were runtime flags here:
/// two-phase confirmation and whether `accept_docs` is present.  Together with
/// [`ScoredCollectorOps`] that leaves the loop body as: advance the iterator,
/// optionally confirm, score, hand over — with no per-document test of the mode.
export template <class ScorerT>
class BasicBulkScorer final : public BulkScorer {
public:
    explicit BasicBulkScorer(std::unique_ptr<ScorerT> scorer) : scorer_(std::move(scorer)) {}

    [[nodiscard]] Result<int32_t> score(LeafCollector &collector, const Bits *accept_docs, int32_t min,
                                        int32_t max) override {
        if (auto set = collector.set_scorer(*scorer_); !set.has_value()) {
            return std::unexpected(std::move(set.error()));
        }
        // Everything below is loop invariant, so it is resolved once per leaf.
        ops_ = collector.scored_ops();
        const bool has_two_phase = scorer_->has_two_phase_iterator() == TwoPhaseState::Yes;

        auto competitive = collector.competitive_iterator();
        if (!competitive.has_value()) {
            return std::unexpected(std::move(competitive.error()));
        }
        const bool has_competitive_iterator = competitive.value() != nullptr;

        if (!has_competitive_iterator && scorer_->approx_doc_id() == -1 && accept_docs == nullptr && min == 0 &&
            max == NO_MORE_DOCS) {
            if (auto scored = score_all(collector, has_two_phase); !scored.has_value()) {
                return std::unexpected(std::move(scored.error()));
            }
            return NO_MORE_DOCS;
        }
        return score_range(collector, accept_docs, min, max, has_competitive_iterator, has_two_phase);
    }

    [[nodiscard]] Result<int64_t> cost() override { return scorer_->cost(); }

private:
    /// How one hit is collected; resolved once per leaf.
    enum Mode : int {
        /// Through [`LeafCollector::collect`], which asks the scorer for the score.
        kPlain = 0,
        /// The score is computed here and handed to the collector's statically
        /// bound entry point ([`ScoredCollectorOps`]).
        kPrecomputed = 1,
        /// The same, with the scorer's branch-free scoring path.
        kFastScore = 2,
    };

    [[nodiscard]] int mode() const noexcept {
        if (!ops_.valid()) {
            return kPlain;
        }
        if constexpr (requires { scorer_->has_fast_score(); }) {
            if (scorer_->has_fast_score()) {
                return kFastScore;
            }
        }
        return kPrecomputed;
    }

    /// Scores and hands one hit to the collector.
    template <int Mode>
    [[nodiscard]] VoidResult hand_to_collector(LeafCollector &collector, int32_t doc) {
        if constexpr (Mode == kPlain) {
            return collector.collect(doc, *scorer_);
        } else {
            Result<float> score = [this]() -> Result<float> {
                if constexpr (Mode == kFastScore && requires { scorer_->fast_score(); }) {
                    return scorer_->fast_score();
                } else {
                    // A type erased scorer has no branch-free path; the mode is
                    // never `kFastScore` for it, but the specialisation is still
                    // instantiated.
                    return scorer_->score();
                }
            }();
            if (!score.has_value()) {
                return std::unexpected(std::move(score.error()));
            }
            return ops_.collect_scored(ops_.self, doc, *scorer_, score.value());
        }
    }

    /// The scorer's document advance, through its inlined entry point when it
    /// has one (`TermScorer::next_doc_inline`): the concrete postings then walk
    /// inside this loop instead of behind two virtual calls.
    [[gnu::always_inline]] [[nodiscard]] static Result<int32_t> next_doc_of(ScorerT &scorer) {
        if constexpr (requires { scorer.next_doc_inline(); }) {
            return scorer.next_doc_inline();
        } else {
            return scorer.next_doc();
        }
    }

    /// See [`next_doc_of`].
    [[gnu::always_inline]] [[nodiscard]] static Result<int32_t> advance_of(ScorerT &scorer, int32_t target) {
        if constexpr (requires { scorer.advance_inline(target); }) {
            return scorer.advance_inline(target);
        } else {
            return scorer.advance(target);
        }
    }

    /// Whether `accept_docs` allows `doc`; only used by the non-specialised paths.
    [[nodiscard]] static Result<bool> is_accept_doc(const Bits *accept_docs, int32_t doc) {
        if (accept_docs == nullptr) {
            return true;
        }
        return accept_docs->get(static_cast<size_t>(doc));
    }

    /// Bulk-scores every hit.  Separate loops per mode keep each of them small
    /// (LUCENE-5487).
    [[nodiscard]] VoidResult score_all(LeafCollector &collector, bool has_two_phase) {
        const int m = mode();
        if (has_two_phase) {
            switch (m) {
                case kFastScore:
                    return score_all_impl<kFastScore, true>(collector);
                case kPrecomputed:
                    return score_all_impl<kPrecomputed, true>(collector);
                default:
                    return score_all_impl<kPlain, true>(collector);
            }
        }
        switch (m) {
            case kFastScore:
                return score_all_impl<kFastScore, false>(collector);
            case kPrecomputed:
                return score_all_impl<kPrecomputed, false>(collector);
            default:
                return score_all_impl<kPlain, false>(collector);
        }
    }

    template <int Mode, bool TwoPhase>
    [[nodiscard]] VoidResult score_all_impl(LeafCollector &collector) {
        for (;;) {
            auto doc = [this]() -> Result<int32_t> {
                if constexpr (TwoPhase) {
                    return scorer_->approx_next_doc();
                } else {
                    return next_doc_of(*scorer_);
                }
            }();
            if (!doc.has_value()) {
                return std::unexpected(std::move(doc.error()));
            }
            if (doc.value() == NO_MORE_DOCS) {
                return {};
            }
            if constexpr (TwoPhase) {
                auto matched = scorer_->matches();
                if (!matched.has_value()) {
                    return std::unexpected(std::move(matched.error()));
                }
                if (!matched.value()) {
                    continue;
                }
            }
            if (auto collected = hand_to_collector<Mode>(collector, doc.value()); !collected.has_value()) {
                return collected;
            }
        }
    }

    /// Bulk-scores the hits in `[min, max)`.
    [[nodiscard]] Result<int32_t> score_range(LeafCollector &collector, const Bits *accept_docs, int32_t min,
                                              int32_t max, bool has_competitive, bool has_two_phase) {
        if (has_competitive) {
            return score_range_competitive(collector, accept_docs, min, max, has_two_phase);
        }
        const int m = mode();
        const bool have_accept = accept_docs != nullptr;
        if (has_two_phase) {
            switch (m) {
                case kFastScore:
                    return have_accept ? score_range_impl<kFastScore, true, true>(collector, accept_docs, min, max)
                                       : score_range_impl<kFastScore, true, false>(collector, accept_docs, min, max);
                case kPrecomputed:
                    return have_accept ? score_range_impl<kPrecomputed, true, true>(collector, accept_docs, min, max)
                                       : score_range_impl<kPrecomputed, true, false>(collector, accept_docs, min, max);
                default:
                    return have_accept ? score_range_impl<kPlain, true, true>(collector, accept_docs, min, max)
                                       : score_range_impl<kPlain, true, false>(collector, accept_docs, min, max);
            }
        }
        switch (m) {
            case kFastScore:
                return have_accept ? score_range_impl<kFastScore, false, true>(collector, accept_docs, min, max)
                                   : score_range_impl<kFastScore, false, false>(collector, accept_docs, min, max);
            case kPrecomputed:
                return have_accept ? score_range_impl<kPrecomputed, false, true>(collector, accept_docs, min, max)
                                   : score_range_impl<kPrecomputed, false, false>(collector, accept_docs, min, max);
            default:
                return have_accept ? score_range_impl<kPlain, false, true>(collector, accept_docs, min, max)
                                   : score_range_impl<kPlain, false, false>(collector, accept_docs, min, max);
        }
    }

    template <int Mode, bool TwoPhase, bool HaveAccept>
    [[nodiscard]] Result<int32_t> score_range_impl(LeafCollector &collector, const Bits *accept_docs, int32_t min,
                                                   int32_t max) {
        int32_t doc = scorer_->approx_doc_id();
        if (doc < min) {
            if (doc == min - 1) {
                auto next = [this]() -> Result<int32_t> {
                    if constexpr (TwoPhase) {
                        return scorer_->approx_next_doc();
                    } else {
                        return next_doc_of(*scorer_);
                    }
                }();
                if (!next.has_value()) {
                    return std::unexpected(std::move(next.error()));
                }
                doc = next.value();
            } else {
                auto advanced = [this, min]() -> Result<int32_t> {
                    if constexpr (TwoPhase) {
                        return scorer_->approx_advance(min);
                    } else {
                        return advance_of(*scorer_, min);
                    }
                }();
                if (!advanced.has_value()) {
                    return std::unexpected(std::move(advanced.error()));
                }
                doc = advanced.value();
            }
        }

        while (doc < max) {
            bool accepted = true;
            if constexpr (HaveAccept) {
                auto accept = accept_docs->get(static_cast<size_t>(doc));
                if (!accept.has_value()) {
                    return std::unexpected(std::move(accept.error()));
                }
                accepted = accept.value();
            }
            if (accepted) {
                bool matched = true;
                if constexpr (TwoPhase) {
                    auto matches = scorer_->matches();
                    if (!matches.has_value()) {
                        return std::unexpected(std::move(matches.error()));
                    }
                    matched = matches.value();
                }
                if (matched) {
                    if (auto collected = hand_to_collector<Mode>(collector, doc); !collected.has_value()) {
                        return std::unexpected(std::move(collected.error()));
                    }
                }
            }

            auto next = [this]() -> Result<int32_t> {
                if constexpr (TwoPhase) {
                    return scorer_->approx_next_doc();
                } else {
                    return next_doc_of(*scorer_);
                }
            }();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
            doc = next.value();
        }
        return doc;
    }

    /// The rare path: the collector can skip documents it cannot compete with,
    /// so every candidate is checked against its competitive iterator.  Kept out
    /// of the specialised loops to keep those small.
    [[nodiscard]] Result<int32_t> score_range_competitive(LeafCollector &collector, const Bits *accept_docs,
                                                          int32_t min, int32_t max, bool has_two_phase) {
        auto competitive = collector.competitive_iterator();
        if (!competitive.has_value()) {
            return std::unexpected(std::move(competitive.error()));
        }
        if (competitive.value() == nullptr) {
            return err(LuceneError::illegal_state("the collector has no competitive iterator"));
        }
        if (competitive.value()->doc_id() > min) {
            min = std::min(competitive.value()->doc_id(), max);
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

        const int m = mode();
        while (doc < max) {
            auto competitive_now = collector.competitive_iterator();
            if (!competitive_now.has_value()) {
                return std::unexpected(std::move(competitive_now.error()));
            }
            if (competitive_now.value() != nullptr) {
                int32_t competitive_doc = competitive_now.value()->doc_id();
                if (competitive_doc < doc) {
                    auto advanced = competitive_now.value()->advance(doc);
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
                VoidResult collected = m == kFastScore   ? hand_to_collector<kFastScore>(collector, doc)
                                       : m == kPrecomputed ? hand_to_collector<kPrecomputed>(collector, doc)
                                                           : hand_to_collector<kPlain>(collector, doc);
                if (!collected.has_value()) {
                    return std::unexpected(std::move(collected.error()));
                }
            }

            auto next = scorer_->approx_next_doc();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
            doc = next.value();
        }
        return doc;
    }

    std::unique_ptr<ScorerT> scorer_;
    /// The collector's statically bound scored entry point; empty when the
    /// collector only implements the abstract interface.
    ScoredCollectorOps ops_{};
};

/// [`BasicBulkScorer`] over the type erased [`Scorer`].
export using DefaultBulkScorer = BasicBulkScorer<Scorer>;

} // namespace lucene
