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

module lucene_core:term_scorer.impl;

import :term_scorer;

import :bm25_similarity;
import :doc_id_set_iterator;
import :lucene101_postings_reader;
import :error;
import :impacts_disi;
import :impacts_enum;
import :max_score_cache;
import :numeric_doc_values;
import :postings_enum;
import :scorable;
import :scorer;
import :similarity;
import :types;

import std;

namespace lucene {

TermScorer::TermScorer(std::unique_ptr<PostingsEnum> postings, std::shared_ptr<SimScorer> sim_scorer,
                       std::unique_ptr<NumericDocValues> norms)
        : postings_(std::move(postings)), sim_scorer_(std::move(sim_scorer)), norms_(std::move(norms)) {
    prepare_fast_path();
}

void TermScorer::prepare_fast_path() {
    // The norms of a document are one load away when the encoding is dense or
    // constant; the sparse encoding keeps the generic path.  A field without
    // norms is the constant case: every document scores with the neutral norm.
    if (norms_ == nullptr) {
        norms_direct_ = true;
    } else if (auto view = norms_->direct_values(); view.has_value()) {
        norms_view_ = view.value();
        norms_direct_ = true;
    }
    // A scorer over a similarity other than BM25 keeps calling into it.  The
    // parameters are cached rather than the formula: the similarity owns them and
    // outlives this scorer.
    if (const auto *const bm25 = dynamic_cast<const BM25Scorer *>(sim_scorer_.get()); bm25 != nullptr) {
        bm25_weight_ = bm25->weight();
        bm25_norm_inverse_ = bm25->norm_inverse_cache();
    }
    if (postings_ != nullptr) {
        freq_source_ = postings_.get();
    }
    if (auto *const concrete = dynamic_cast<BlockPostingsEnum *>(freq_source_); concrete != nullptr) {
        freq_source_concrete_ = concrete;
    }
    fast_score_ok_ = bm25_norm_inverse_ != nullptr && norms_direct_;
}

Result<std::unique_ptr<TermScorer>> TermScorer::from_impacts(std::unique_ptr<ImpactsEnum> impacts_enum,
                                                            std::shared_ptr<SimScorer> sim_scorer,
                                                            std::unique_ptr<NumericDocValues> norms,
                                                            bool top_level_scoring_clause) {
    if (impacts_enum == nullptr) {
        return err(LuceneError::illegal_argument("an impacts term scorer needs an impacts enum"));
    }
    std::unique_ptr<TermScorer> scorer(new TermScorer(nullptr, std::move(sim_scorer), std::move(norms)));
    scorer->impacts_enum_ = std::move(impacts_enum);
    // `freq` reads the impacts enum when there is one; resolved here so the
    // scoring path has no choice to make.
    scorer->freq_source_ = scorer->impacts_enum_.get();
    if (auto *const concrete = dynamic_cast<BlockPostingsEnum *>(scorer->freq_source_); concrete != nullptr) {
        scorer->freq_source_concrete_ = concrete;
    }
    scorer->max_score_cache_ = std::make_unique<MaxScoreCache>(scorer->impacts_enum_.get(), scorer->sim_scorer_);
    if (top_level_scoring_clause) {
        // Iterating through the impacts iterator is what lets the collector's
        // minimum competitive score prune blocks.
        auto disi = ImpactsDISI::from_source(scorer->max_score_cache_.get());
        if (!disi.has_value()) {
            return std::unexpected(std::move(disi.error()));
        }
        scorer->impacts_disi_ = std::move(disi.value());
    }
    return scorer;
}

int32_t TermScorer::doc_id() const { return current_doc(); }

Result<int32_t> TermScorer::next_doc() {
    auto next = cursor().next_doc();
    if (next.has_value()) {
        doc_ = next.value();
    }
    return next;
}

Result<int32_t> TermScorer::advance(int32_t target) {
    auto advanced = cursor().advance(target);
    if (advanced.has_value()) {
        doc_ = advanced.value();
    }
    return advanced;
}

Result<int32_t> TermScorer::slow_advance(int32_t target) {
    auto advanced = cursor().slow_advance(target);
    if (advanced.has_value()) {
        doc_ = advanced.value();
    }
    return advanced;
}

Result<int64_t> TermScorer::cost() const { return cursor().cost(); }

Result<int32_t> TermScorer::freq() {
    if (freq_source_concrete_ != nullptr) {
        // Statically dispatched: `BlockPostingsEnum` is final, so this is a
        // direct call that inlines into the scoring loop.
        return static_cast<BlockPostingsEnum *>(freq_source_concrete_)->freq();
    }
    if (freq_source_ != nullptr) {
        return freq_source_->freq();
    }
    return postings_->freq();
}

Result<float> TermScorer::get_max_score(int32_t upto) {
    if (max_score_cache_ == nullptr) {
        // A valid upper bound that only disables pruning.
        return std::numeric_limits<float>::infinity();
    }
    return max_score_cache_->get_max_score(upto);
}

Result<int32_t> TermScorer::advance_shallow(int32_t target) {
    if (max_score_cache_ == nullptr) {
        return NO_MORE_DOCS;
    }
    return max_score_cache_->advance_shallow(target);
}

VoidResult TermScorer::set_min_competitive_score(float min_score) {
    if (impacts_disi_ != nullptr) {
        impacts_disi_->set_min_competitive_score(min_score);
    }
    return {};
}

Result<std::pair<int32_t, int64_t>> TermScorer::freq_and_norm() {
    int64_t norm = 1;
    if (norms_ != nullptr) {
        auto exact = norms_->advance_exact(doc_id());
        if (!exact.has_value()) {
            return std::unexpected(std::move(exact.error()));
        }
        if (exact.value()) {
            auto value = norms_->long_value();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            norm = value.value();
        }
    }
    auto frequency = freq();
    if (!frequency.has_value()) {
        return std::unexpected(std::move(frequency.error()));
    }
    return std::pair<int32_t, int64_t>{frequency.value(), norm};
}

Result<float> TermScorer::fast_score() {
    auto frequency = freq();
    if (!frequency.has_value()) {
        return std::unexpected(std::move(frequency.error()));
    }
    int64_t norm = 1;
    if (!norms_view_.value_at(current_doc(), norm)) {
        // The document has no norm; the neutral value scores as an empty field.
        norm = 1;
    }
    return bm25_tf_score(bm25_weight_, bm25_norm_inverse_[static_cast<uint8_t>(norm)],
                         static_cast<float>(frequency.value()));
}

Result<float> TermScorer::score() {
    if (fast_score_ok_) {
        return fast_score();
    }
    if (bm25_norm_inverse_ != nullptr) {
        // The direct path: one virtual call for the frequency, one load for the
        // norm, the BM25 formula inline.  **Deviation:** the reference
        // implementation gets the same effect from monomorphising the whole
        // collector over the concrete scorer types.
        auto frequency = freq();
        if (!frequency.has_value()) {
            return std::unexpected(std::move(frequency.error()));
        }
        int64_t norm = 1;
        if (norms_direct_) {
            if (!norms_view_.value_at(current_doc(), norm)) {
                // The document has no norm; the neutral value scores as an empty
                // field.
                norm = 1;
            }
        } else if (norms_ != nullptr) {
            auto exact = norms_->exact_value(current_doc(), norm);
            if (!exact.has_value()) {
                return std::unexpected(std::move(exact.error()));
            }
            if (!exact.value()) {
                norm = 1;
            }
        }
        return bm25_tf_score(bm25_weight_, bm25_norm_inverse_[static_cast<uint8_t>(norm)],
                             static_cast<float>(frequency.value()));
    }
    auto pair = freq_and_norm();
    if (!pair.has_value()) {
        return std::unexpected(std::move(pair.error()));
    }
    return sim_scorer_->score(static_cast<float>(pair.value().first), pair.value().second);
}

Result<float> TermScorer::smoothing_score(int32_t doc) {
    int64_t norm = 1;
    if (norms_ != nullptr) {
        auto exact = norms_->advance_exact(doc);
        if (!exact.has_value()) {
            return std::unexpected(std::move(exact.error()));
        }
        if (exact.value()) {
            auto value = norms_->long_value();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            norm = value.value();
        }
    }
    return sim_scorer_->score(0.0F, norm);
}

} // namespace lucene
