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

export module lucene_core:max_score_cache;

import :types;
import std;

import :bm25_similarity;
import :error;
import :impact;
import :impacts;
import :impacts_source;
import :similarity;

namespace lucene {

/// Computes maximum scores from [`Impacts`] and caches them, so the similarity
/// never scores the same impact list twice.
///
/// Port of `MaxScoreCache` (`core/search/max_score_cache.rs`).
///
/// **Deviation:** the reference implementation owns the [`ImpactsSource`]; here
/// the cache borrows it, because the owning scorers (`TermScorer`, and later the
/// conjunction scorers) already own their [`ImpactsEnum`] and their state cannot
/// be moved out of it.  The source must outlive the cache.
export class MaxScoreCache {
public:
    /// `scorer` is only read, so a scorer borrowed from a weight can be shared
    /// with the scorers that delegate their maximum score here.
    MaxScoreCache(ImpactsSource *impacts_source, std::shared_ptr<const SimScorer> scorer);

    MaxScoreCache(const MaxScoreCache &) = delete;
    MaxScoreCache &operator=(const MaxScoreCache &) = delete;
    MaxScoreCache(MaxScoreCache &&) = delete;
    MaxScoreCache &operator=(MaxScoreCache &&) = delete;
    ~MaxScoreCache() = default;

    /// The source this cache reads impacts from.
    [[nodiscard]] ImpactsSource &impacts_source() noexcept { return *impacts_source_; }
    [[nodiscard]] const ImpactsSource &impacts_source() const noexcept { return *impacts_source_; }

    /// The similarity scorer used to turn impacts into scores.
    [[nodiscard]] const SimScorer &sim_scorer() const noexcept { return *scorer_; }

    /// The score of the impact with the largest frequency and the smallest norm;
    /// the upper bound that holds when no level covers the requested documents.
    [[nodiscard]] float global_max_score() const noexcept { return global_max_score_; }

    /// The contract of `Scorer::advance_shallow`, implemented on top of the
    /// wrapped source: shallow-advances and answers the last document of the
    /// block that was reached.
    [[nodiscard]] Result<int32_t> advance_shallow(int32_t target);

    /// The maximum score of the documents up to `upto` included, from the
    /// coarsest level that covers them, or the global maximum score when no
    /// level does.
    [[nodiscard]] Result<float> get_max_score(int32_t upto);

    /// [`get_max_score`] of level 0; every caller that iterates documents only
    /// needs the finest level.
    [[nodiscard]] Result<float> get_max_score_with_level_zero();

    /// [`advance_shallow`] followed by [`get_max_score_with_level_zero`] on the
    /// block that was reached, in one round trip: the document iterators ask for
    /// exactly this pair on entering every block.
    [[nodiscard]] Result<int32_t> advance_shallow_and_block_max(int32_t target, float &max_score);

    /// An inclusive upper bound of documents whose score is known to be below
    /// `min_score`, or `-1` when the current document may still be competitive.
    [[nodiscard]] Result<int32_t> get_skip_up_to(float min_score);

private:
    struct CacheEntry {
        float score{0.0F};
        int32_t upto{-1};
    };

    [[nodiscard]] VoidResult ensure_cache_size(size_t size);

    /// The impacts of the current block.
    ///
    /// `ImpactsSource::get_impacts` builds two optionals, two spans and passes
    /// six arguments on every call, and the skip logic asks for the impacts
    /// three or four times per block; the object itself is reused in place, so
    /// the pointer is cached and only refreshed when the block moves.
    [[nodiscard]] Result<Impacts *> current_impacts() const;

    /// Re-publishes the current block to the impacts and caches the object.
    ///
    /// `ImpactsSource::get_impacts` is what pushes the new block's serialized
    /// impacts into the source, so it has to be called once per block; every
    /// other read in this cache then uses the cached pointer.
    [[nodiscard]] Result<Impacts *> refresh_impacts();

    /// The first level that covers `upto`, if any.
    [[nodiscard]] Result<std::optional<size_t>> get_level(int32_t upto) const;

    [[nodiscard]] Result<float> get_max_score_with_level(size_t level);

    /// The largest level whose maximum score is still below `min_score`.
    [[nodiscard]] Result<std::optional<size_t>> get_skip_level(float min_score);

    /// The score of one impact.
    ///
    /// When the similarity is BM25 — the default, and the one every scoring loop
    /// in practice runs with — the coefficients are hoisted out of the similarity
    /// object once, so scoring an impact is a multiply-add instead of a virtual
    /// call per impact and per block.
    [[nodiscard]] float score_of(const Impact &impact) const noexcept {
        if (bm25_norm_inverse_ != nullptr) {
            return bm25_tf_score(bm25_weight_, bm25_norm_inverse_[static_cast<uint8_t>(impact.norm)],
                                 static_cast<float>(impact.freq));
        }
        return scorer_->score(static_cast<float>(impact.freq), impact.norm);
    }

    ImpactsSource *impacts_source_;
    std::shared_ptr<const SimScorer> scorer_;
    float global_max_score_;
    /// Hoisted BM25 coefficients; `bm25_norm_inverse_` is `nullptr` for every
    /// other similarity.
    float bm25_weight_{0.0F};
    const float *bm25_norm_inverse_{nullptr};
    /// The impacts of the current block; see [`current_impacts`].
    mutable Impacts *impacts_{nullptr};
    /// The score cache, one entry per impacts level.
    std::vector<CacheEntry> cache_;
};

} // namespace lucene
