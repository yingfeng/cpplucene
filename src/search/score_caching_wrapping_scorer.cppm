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

export module lucene_core:score_caching_wrapping_scorer;

import :types;
import std;

import :doc_id_set_iterator;
import :error;
import :leaf_collector;
import :scorable;

namespace lucene {

/// A [`Scorable`] that caches the score of the current document, so that
/// successive calls to [`score`] answer the same value without scoring again.
///
/// Port of `ScoreCachingWrappingScorer`
/// (`core/search/score_caching_wrapping_scorer.rs`).
///
/// Scores are only computed when a collector asks for them, so a collector that
/// needs the score in several places would otherwise score the same document
/// more than once.  The cache is invalidated by
/// [`ScoreCachingLeafCollector::collect`] and
/// [`ScoreCachingLeafCollector::set_scorer`].
///
/// **Deviation:** the reference implementation shares one cache cell between the
/// scorer and its leaf collector through a reference; the port puts the cache in
/// the leaf collector and hands the scorer a non-owning pointer to it, which is
/// the same single-threaded contract.  `cost()` is unsupported in the reference
/// implementation as well.
export class CachedScore {
public:
    /// Forget the cached score; the next [`score`] call recomputes it.
    void invalidate() noexcept { cached_ = false; }

    /// Whether a score is currently cached.
    [[nodiscard]] bool is_cached() const noexcept { return cached_; }

    /// The cached score, meaningless unless [`is_cached`] holds.
    [[nodiscard]] float score() const noexcept { return score_; }

    void set_score(float score) noexcept {
        score_ = score;
        cached_ = true;
    }

private:
    bool cached_{false};
    float score_{0.0F};
};

/// A [`Scorable`] around another one that caches the score of the current
/// document in `cache`.
export class ScoreCachingWrappingScorer final : public Scorable {
public:
    /// `cache` must outlive the wrapper, which is the case when both live in the
    /// same leaf collector.
    ScoreCachingWrappingScorer(Scorable &in, CachedScore &cache) : in_(in), cache_(cache) {}

    [[nodiscard]] Result<float> score() override {
        if (!cache_.is_cached()) {
            auto computed = in_.score();
            if (!computed.has_value()) {
                return std::unexpected(std::move(computed.error()));
            }
            cache_.set_score(computed.value());
        }
        return cache_.score();
    }

    [[nodiscard]] VoidResult set_min_competitive_score(float min_score) override {
        return in_.set_min_competitive_score(min_score);
    }

    [[nodiscard]] Result<std::vector<ChildScorable>> get_children() override { return in_.get_children(); }

private:
    Scorable &in_;
    CachedScore &cache_;
};

/// A [`LeafCollector`] that hands its inner collector a
/// [`ScoreCachingWrappingScorer`], so repeated `score()` calls of one document
/// are computed only once.
export class ScoreCachingLeafCollector final : public LeafCollector {
public:
    explicit ScoreCachingLeafCollector(std::unique_ptr<LeafCollector> inner) : inner_(std::move(inner)) {}

    [[nodiscard]] VoidResult set_scorer(Scorable &scorer) override {
        cache_.invalidate();
        ScoreCachingWrappingScorer wrapper(scorer, cache_);
        return inner_->set_scorer(wrapper);
    }

    [[nodiscard]] VoidResult collect(int32_t doc, Scorable &scorer) override {
        ScoreCachingWrappingScorer wrapper(scorer, cache_);
        // The cached score belongs to the previous document.
        cache_.invalidate();
        return inner_->collect(doc, wrapper);
    }

    [[nodiscard]] Result<std::unique_ptr<DocIdSetIterator>> competitive_iterator() override {
        return inner_->competitive_iterator();
    }

    [[nodiscard]] VoidResult finish() override { return inner_->finish(); }

    [[nodiscard]] std::string to_string() const override { return inner_->to_string(); }

    /// The wrapped collector.
    [[nodiscard]] LeafCollector &inner() noexcept { return *inner_; }

private:
    std::unique_ptr<LeafCollector> inner_;
    CachedScore cache_;
};

} // namespace lucene
