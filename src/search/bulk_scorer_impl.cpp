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

module lucene_core:bulk_scorer.impl;

import :bulk_scorer;

import :bits;
import :doc_id_set_iterator;
import :error;
import :leaf_collector;
import :scorable;
import :scorer;
import :types;

import std;

namespace lucene {

namespace {

/// Whether `accept_docs` allows `doc`.
[[nodiscard]] Result<bool> is_accept_doc(const Bits *accept_docs, int32_t doc) {
    if (accept_docs == nullptr) {
        return true;
    }
    return accept_docs->get(static_cast<size_t>(doc));
}

/// Bulk-scores every hit.  Separate from [`score_range`] to keep the hot loop
/// small (LUCENE-5487).
[[nodiscard]] VoidResult score_all(LeafCollector &collector, const Bits *accept_docs, Scorer &scorer,
                                   bool has_two_phase) {
    if (!has_two_phase) {
        for (;;) {
            auto doc = scorer.next_doc();
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
                if (auto collected = collector.collect(doc.value(), scorer); !collected.has_value()) {
                    return collected;
                }
            }
        }
    }

    // The scorer has an approximation: run it first, then check acceptDocs, then
    // confirm.
    for (;;) {
        auto doc = scorer.approx_next_doc();
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
            auto matched = scorer.matches();
            if (!matched.has_value()) {
                return std::unexpected(std::move(matched.error()));
            }
            if (matched.value()) {
                if (auto collected = collector.collect(doc.value(), scorer); !collected.has_value()) {
                    return collected;
                }
            }
        }
    }
}

/// Bulk-scores the hits in `[min, max)`.  Separate from [`score_all`] for the
/// same reason.
[[nodiscard]] Result<int32_t> score_range(LeafCollector &collector, const Bits *accept_docs, int32_t min,
                                          int32_t max, Scorer &scorer, bool has_competitive,
                                          bool has_two_phase) {
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

    int32_t doc = scorer.approx_doc_id();
    if (doc < min) {
        if (doc == min - 1) {
            auto next = scorer.approx_next_doc();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
            doc = next.value();
        } else {
            auto advanced = scorer.approx_advance(min);
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
                if (auto collected = collector.collect(doc, scorer); !collected.has_value()) {
                    return std::unexpected(std::move(collected.error()));
                }
            }
            auto next = scorer.next_doc();
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
                        auto advanced = scorer.approx_advance(competitive_doc);
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
                auto matches = scorer.matches();
                if (!matches.has_value()) {
                    return std::unexpected(std::move(matches.error()));
                }
                matched = matches.value();
            }
            if (accept.value() && matched) {
                if (auto collected = collector.collect(doc, scorer); !collected.has_value()) {
                    return std::unexpected(std::move(collected.error()));
                }
            }

            auto next = scorer.approx_next_doc();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
            doc = next.value();
        }
    }

    return doc;
}

} // namespace

DefaultBulkScorer::DefaultBulkScorer(std::unique_ptr<Scorer> scorer) : scorer_(std::move(scorer)) {}

Result<int32_t> DefaultBulkScorer::score(LeafCollector &collector, const Bits *accept_docs, int32_t min,
                                         int32_t max) {
    if (auto set = collector.set_scorer(*scorer_); !set.has_value()) {
        return std::unexpected(std::move(set.error()));
    }
    const bool has_two_phase = scorer_->has_two_phase_iterator() == TwoPhaseState::Yes;
    const int32_t doc_id = scorer_->approx_doc_id();

    auto competitive = collector.competitive_iterator();
    if (!competitive.has_value()) {
        return std::unexpected(std::move(competitive.error()));
    }
    const bool has_competitive_iterator = competitive.value() != nullptr;

    if (!has_competitive_iterator && doc_id == -1 && accept_docs == nullptr && min == 0 && max == NO_MORE_DOCS) {
        if (auto scored = score_all(collector, accept_docs, *scorer_, has_two_phase); !scored.has_value()) {
            return std::unexpected(std::move(scored.error()));
        }
        return NO_MORE_DOCS;
    }
    return score_range(collector, accept_docs, min, max, *scorer_, has_competitive_iterator, has_two_phase);
}

Result<int64_t> DefaultBulkScorer::cost() { return scorer_->cost(); }

} // namespace lucene
