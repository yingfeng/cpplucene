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

export module lucene_core:impacts_disi;

import :types;
import std;

import :doc_id_set_iterator;
import :error;
import :max_score_cache;

namespace lucene {

/// A [`DocIdSetIterator`] that skips non-competitive documents thanks to the
/// indexed impacts.
///
/// Port of `ImpactsDISI` (`core/search/impacts_disi.rs`).  Call
/// [`set_min_competitive_score`] to give it the ability to skip low-scoring
/// documents; without it the iterator only walks the documents.
///
/// **Deviation:** the reference implementation models the two roles (iterating
/// a separate iterator, or iterating the impacts source itself) as two generic
/// modes; here one class covers both and borrows the iterator and the cache —
/// the owning scorer keeps them alive.
export class ImpactsDISI final : public DocIdSetIterator {
public:
    /// Iterates `iterator`, using the impacts of `cache` to skip blocks.
    ImpactsDISI(DocIdSetIterator *iterator, MaxScoreCache *cache);

    /// Iterates the impacts source of `cache` itself, which must also be a
    /// [`DocIdSetIterator`] (every ported impacts source is an `ImpactsEnum`).
    [[nodiscard]] static Result<std::unique_ptr<ImpactsDISI>> from_source(MaxScoreCache *cache);

    /// The cache this iterator reads its impacts from.
    [[nodiscard]] MaxScoreCache &max_score_cache() noexcept { return *cache_; }
    [[nodiscard]] const MaxScoreCache &max_score_cache() const noexcept { return *cache_; }

    /// The iterator whose documents are visited, without competitive skipping.
    [[nodiscard]] DocIdSetIterator &iterator() noexcept { return *iterator_; }

    /// Tells the iterator that documents scoring below `min_score` may be
    /// skipped.  The score must not decrease between calls.
    void set_min_competitive_score(float min_score) noexcept;

    /// The contract of `Scorer::advance_shallow`.
    [[nodiscard]] Result<int32_t> advance_shallow(int32_t target) { return cache_->advance_shallow(target); }

    /// The contract of `Scorer::get_max_score`.
    [[nodiscard]] Result<float> get_max_score(int32_t upto) { return cache_->get_max_score(upto); }

    // ---- DocIdSetIterator ----

    [[nodiscard]] int32_t doc_id() const override { return iterator_->doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override { return iterator_->cost(); }

    // ---- the same walk over a statically typed upstream ----

    /// [`next_doc`] and [`advance`] over `upstream`, which must be the concrete
    /// type of [`iterator`].
    ///
    /// The document calls of the abstract path are two virtual calls per
    /// document (this class to the upstream, and the upstream's own); with the
    /// concrete postings they inline into the caller's scoring loop instead,
    /// which is what the reference implementation gets from monomorphising.
    /// Only the block boundary work (`advance_target`) stays out of line.
    template <class UpstreamT>
    [[gnu::always_inline]] Result<int32_t> next_doc_with(UpstreamT &upstream) {
        const int32_t doc = upstream.doc_id();
        if (doc < upto_) {
            return upstream.next_doc();
        }
        return advance_with(upstream, doc + 1);
    }

    /// See [`next_doc_with`].
    template <class UpstreamT>
    [[gnu::always_inline]] Result<int32_t> advance_with(UpstreamT &upstream, int32_t target) {
        auto doc = advance_target(target);
        if (!doc.has_value()) {
            return std::unexpected(std::move(doc.error()));
        }
        return upstream.advance(doc.value());
    }

    /// The last document of the competitive block, `NO_MORE_DOCS` before the
    /// first block, `-1` when the block has to be recomputed.
    [[nodiscard]] int32_t upto() const noexcept { return upto_; }

private:
    /// Skips blocks whose maximum score is below the minimum competitive score;
    /// answers the document to advance to.
    [[nodiscard]] Result<int32_t> advance_target(int32_t target);

    DocIdSetIterator *iterator_;
    MaxScoreCache *cache_;
    float min_competitive_score_{0.0F};
    /// The last document of the block that was reached, `-1` when the cache has
    /// to be recomputed.
    int32_t upto_{NO_MORE_DOCS};
    /// The maximum score of the current block.
    float max_score_{std::numeric_limits<float>::max()};
};

} // namespace lucene
