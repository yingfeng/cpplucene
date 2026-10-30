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

module lucene_core:impacts_disi.impl;

import :impacts_disi;

import std;

import :debug;
import :doc_id_set_iterator;
import :error;
import :max_score_cache;
import :types;

namespace lucene {

ImpactsDISI::ImpactsDISI(DocIdSetIterator *iterator, MaxScoreCache *cache)
        : iterator_(iterator), cache_(cache) {
    debug_check(iterator_ != nullptr, "an impacts iterator needs a document iterator");
    debug_check(cache_ != nullptr, "an impacts iterator needs a score cache");
}

Result<std::unique_ptr<ImpactsDISI>> ImpactsDISI::from_source(MaxScoreCache *cache) {
    if (cache == nullptr) {
        return err(LuceneError::illegal_argument("an impacts iterator needs a score cache"));
    }
    auto *iterator = dynamic_cast<DocIdSetIterator *>(&cache->impacts_source());
    if (iterator == nullptr) {
        return err(LuceneError::illegal_argument(
                "the impacts source of a source-mode impacts iterator must also iterate documents"));
    }
    std::unique_ptr<ImpactsDISI> disi(new ImpactsDISI(iterator, cache));
    return disi;
}

void ImpactsDISI::set_min_competitive_score(float min_score) noexcept {
    debug_check(min_score >= min_competitive_score_, "the minimum competitive score must not decrease");
    if (min_score > min_competitive_score_) {
        min_competitive_score_ = min_score;
        // Force `upto` and `max_score` to be recomputed, so that documents are
        // skipped only after the minimum competitive score actually increased.
        upto_ = -1;
    }
}

Result<int32_t> ImpactsDISI::advance_target(int32_t target) {
    if (target <= upto_) {
        // Still in the current block, which the impacts consider competitive.
        return target;
    }
    auto shallow = cache_->advance_shallow_and_block_max(target, max_score_);
    if (!shallow.has_value()) {
        return std::unexpected(std::move(shallow.error()));
    }
    upto_ = shallow.value();

    for (;;) {
        if (upto_ < target) {
            // The impacts no longer cover `target`: everything that is left is
            // non-competitive.  **Deviation:** the reference implementation
            // asserts here instead, which assumes the impacts always reach the
            // requested document (true for codec impacts, not for a source that
            // reports the whole postings list as its last block).
            return NO_MORE_DOCS;
        }

        if (max_score_ >= min_competitive_score_) {
            return target;
        }
        if (upto_ == NO_MORE_DOCS) {
            return NO_MORE_DOCS;
        }

        auto skip_up_to = cache_->get_skip_up_to(min_competitive_score_);
        if (!skip_up_to.has_value()) {
            return std::unexpected(std::move(skip_up_to.error()));
        }
        if (skip_up_to.value() == -1) {
            // Nothing further to skip.
            target = upto_ + 1;
        } else if (skip_up_to.value() == NO_MORE_DOCS) {
            return NO_MORE_DOCS;
        } else {
            target = skip_up_to.value() + 1;
        }

        auto next_shallow = cache_->advance_shallow_and_block_max(target, max_score_);
        if (!next_shallow.has_value()) {
            return std::unexpected(std::move(next_shallow.error()));
        }
        upto_ = next_shallow.value();
    }
}

Result<int32_t> ImpactsDISI::next_doc() {
    const int32_t doc = iterator_->doc_id();
    if (doc < upto_) {
        return iterator_->next_doc();
    }
    return advance(doc + 1);
}

Result<int32_t> ImpactsDISI::advance(int32_t target) {
    auto doc = advance_target(target);
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    return iterator_->advance(doc.value());
}

} // namespace lucene
