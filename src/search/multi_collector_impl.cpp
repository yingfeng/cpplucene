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

module lucene_core:multi_collector.impl;

import :multi_collector;

import std;

import :collector;
import :doc_id_set_iterator;
import :error;
import :index_searcher;
import :leaf_collector;
import :leaf_reader_context;
import :scorable;
import :score_mode;
import :types;
import :weight;

namespace lucene {

namespace {

/// A [`Scorable`] that forwards everything but the minimum competitive score,
/// which it swallows.
///
/// Port of `FilterScorable` (`core/search/filter_scorable.rs`).  Wrapping the
/// shared scorer with it keeps one collector's threshold from making the scorer
/// skip documents another collector still needs.
class FilterScorable final : public Scorable {
public:
    explicit FilterScorable(Scorable &in) : in_(in) {}

    [[nodiscard]] Result<float> score() override { return in_.score(); }
    [[nodiscard]] Result<float> smoothing_score(int32_t doc_id) override { return in_.smoothing_score(doc_id); }
    [[nodiscard]] VoidResult set_score(float score) override { return in_.set_score(score); }
    [[nodiscard]] Result<std::vector<ChildScorable>> get_children() override { return in_.get_children(); }

private:
    Scorable &in_;
};

/// A [`Scorable`] that remembers the minimum competitive score of one collector
/// and forwards the smallest one any collector asked for.
///
/// Port of `MinCompetitiveScoreAwareScorable` (`core/search/multi_collector.rs`).
/// Only a score that no collector cares about may be skipped, which is the
/// minimum over all of them.
class MinCompetitiveScoreAwareScorable final : public Scorable {
public:
    MinCompetitiveScoreAwareScorable(Scorable &in, size_t idx, std::vector<float> &min_scores)
            : in_(in), idx_(idx), min_scores_(min_scores) {}

    [[nodiscard]] Result<float> score() override { return in_.score(); }
    [[nodiscard]] Result<float> smoothing_score(int32_t doc_id) override { return in_.smoothing_score(doc_id); }
    [[nodiscard]] Result<std::vector<ChildScorable>> get_children() override { return in_.get_children(); }

    [[nodiscard]] VoidResult set_min_competitive_score(float min_score) override {
        if (min_score <= min_scores_[idx_]) {
            return {};
        }
        min_scores_[idx_] = min_score;
        return in_.set_min_competitive_score(smallest_min_score());
    }

private:
    /// The smallest threshold any collector asked for.
    [[nodiscard]] float smallest_min_score() const {
        float smallest = std::numeric_limits<float>::max();
        for (const float min_score : min_scores_) {
            smallest = std::min(smallest, min_score);
        }
        return smallest;
    }

    Scorable &in_;
    size_t idx_{0};
    std::vector<float> &min_scores_;
};

/// Fans every hit out to the leaf collectors of the wrapped collectors.
class MultiLeafCollector final : public LeafCollector {
public:
    MultiLeafCollector(std::vector<std::unique_ptr<LeafCollector>> collectors, bool skip_non_competitive_scores)
            : collectors_(std::move(collectors)), skip_non_competitive_scores_(skip_non_competitive_scores) {
        if (skip_non_competitive_scores_) {
            min_scores_.assign(collectors_.size(), 0.0F);
        }
    }

    [[nodiscard]] VoidResult set_scorer(Scorable &scorer) override {
        for (size_t idx = 0; idx < collectors_.size(); ++idx) {
            if (collectors_[idx] == nullptr) {
                continue;
            }
            if (skip_non_competitive_scores_) {
                MinCompetitiveScoreAwareScorable aware(scorer, idx, min_scores_);
                if (auto set = collectors_[idx]->set_scorer(aware); !set.has_value()) {
                    return std::unexpected(std::move(set.error()));
                }
            } else {
                FilterScorable filtered(scorer);
                if (auto set = collectors_[idx]->set_scorer(filtered); !set.has_value()) {
                    return std::unexpected(std::move(set.error()));
                }
            }
        }
        return {};
    }

    [[nodiscard]] VoidResult collect(int32_t doc, Scorable &scorer) override {
        for (size_t idx = 0; idx < collectors_.size(); ++idx) {
            if (collectors_[idx] == nullptr) {
                continue;
            }
            auto collected = collect_one(idx, doc, scorer);
            if (!collected.has_value()) {
                if (collected.error().kind() == ErrorKind::kCollectionTerminated) {
                    // Every collector is done: the leaf is finished.
                    return collected;
                }
                return std::unexpected(std::move(collected.error()));
            }
        }
        return {};
    }

    /// Hands `doc` to one collector; a collector that terminated its collection
    /// is finished and dropped, so the others keep going.
    [[nodiscard]] VoidResult collect_one(size_t idx, int32_t doc, Scorable &scorer) {
        VoidResult collected;
        if (skip_non_competitive_scores_) {
            MinCompetitiveScoreAwareScorable aware(scorer, idx, min_scores_);
            collected = collectors_[idx]->collect(doc, aware);
        } else {
            FilterScorable filtered(scorer);
            collected = collectors_[idx]->collect(doc, filtered);
        }
        if (collected.has_value()) {
            return {};
        }
        if (collected.error().kind() != ErrorKind::kCollectionTerminated) {
            return collected;
        }
        if (auto finished = collectors_[idx]->finish(); !finished.has_value()) {
            return finished;
        }
        collectors_[idx].reset();
        const bool any_left = std::any_of(collectors_.begin(), collectors_.end(),
                                          [](const std::unique_ptr<LeafCollector> &collector) {
                                              return collector != nullptr;
                                          });
        if (any_left) {
            return {};
        }
        return err(LuceneError::collection_terminated(""));
    }

    [[nodiscard]] VoidResult finish() override {
        for (std::unique_ptr<LeafCollector> &collector : collectors_) {
            if (collector == nullptr) {
                continue;
            }
            if (auto finished = collector->finish(); !finished.has_value()) {
                return std::unexpected(std::move(finished.error()));
            }
        }
        return {};
    }

    [[nodiscard]] std::string to_string() const override { return "MultiLeafCollector"; }

private:
    std::vector<std::unique_ptr<LeafCollector>> collectors_;
    /// Whether every wrapped collector may skip low-scoring hits, which is what
    /// allows the smallest of their thresholds to reach the scorer.
    bool skip_non_competitive_scores_{false};
    /// The threshold each collector asked for; empty when they cannot skip.
    std::vector<float> min_scores_;
};

} // namespace

MultiCollector::MultiCollector(std::vector<Collector *> collectors) : collectors_(std::move(collectors)) {
    // The combined mode has to satisfy every wrapped collector: scores are
    // collected as soon as one of them needs them, and the search has to be
    // exhaustive as soon as one of them cannot skip documents.
    bool needs_scores = false;
    bool exhaustive = true;
    for (const Collector *collector : collectors_) {
        const ScoreMode mode = collector->score_mode();
        needs_scores = needs_scores || score_mode_needs_scores(mode);
        exhaustive = exhaustive && score_mode_is_exhaustive(mode);
    }
    if (needs_scores) {
        score_mode_ = exhaustive ? ScoreMode::Complete : ScoreMode::TopScores;
    } else {
        score_mode_ = exhaustive ? ScoreMode::CompleteNoScores : ScoreMode::TopDocs;
    }
    // Only when every collector is allowed to skip low-scoring hits may the
    // scorer be told about their thresholds at all.
    skip_non_competitive_scores_ = std::all_of(collectors_.begin(), collectors_.end(), [](const Collector *collector) {
        return collector->score_mode() == ScoreMode::TopScores;
    });
}

Result<std::unique_ptr<MultiCollector>> MultiCollector::wrap(std::vector<Collector *> collectors) {
    if (collectors.empty()) {
        return err(LuceneError::illegal_argument("MultiCollector requires at least one collector"));
    }
    return std::unique_ptr<MultiCollector>(new MultiCollector(std::move(collectors)));
}

Result<std::unique_ptr<LeafCollector>> MultiCollector::get_leaf_collector(const LeafReaderContext &context,
                                                                         const Weight *weight,
                                                                         const IndexSearcher &searcher) {
    std::vector<std::unique_ptr<LeafCollector>> leaves;
    leaves.reserve(collectors_.size());
    for (Collector *collector : collectors_) {
        auto leaf = collector->get_leaf_collector(context, weight, searcher);
        if (!leaf.has_value()) {
            if (leaf.error().kind() == ErrorKind::kCollectionTerminated) {
                // The collector answered this leaf without visiting it (a counter
                // whose weight knows the count): it takes no leaf collector here.
                leaves.push_back(nullptr);
                continue;
            }
            return std::unexpected(std::move(leaf.error()));
        }
        leaves.push_back(std::move(leaf.value()));
    }
    const bool any_left = std::any_of(leaves.begin(), leaves.end(),
                                      [](const std::unique_ptr<LeafCollector> &collector) {
                                          return collector != nullptr;
                                      });
    if (!any_left) {
        return err(LuceneError::collection_terminated("all collectors answered the leaf without collecting"));
    }
    std::unique_ptr<LeafCollector> leaf(new MultiLeafCollector(std::move(leaves), skip_non_competitive_scores_));
    return leaf;
}

VoidResult MultiCollector::set_weight(const Weight *weight) {
    for (Collector *collector : collectors_) {
        if (auto set = collector->set_weight(weight); !set.has_value()) {
            return std::unexpected(std::move(set.error()));
        }
    }
    return {};
}

} // namespace lucene
