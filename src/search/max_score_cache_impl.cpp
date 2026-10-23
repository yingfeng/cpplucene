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

module lucene_core:max_score_cache.impl;

import :max_score_cache;

import std;

import :error;
import :impact;
import :impacts;
import :impacts_source;
import :similarity;
import :types;

namespace lucene {

MaxScoreCache::MaxScoreCache(ImpactsSource *impacts_source, std::shared_ptr<SimScorer> scorer)
        : impacts_source_(impacts_source),
          scorer_(std::move(scorer)),
          global_max_score_(scorer_->score(std::numeric_limits<float>::max(), 1)) {}

Result<int32_t> MaxScoreCache::advance_shallow(int32_t target) {
    auto advanced = impacts_source_->advance_shallow(target);
    if (!advanced.has_value()) {
        return std::unexpected(std::move(advanced.error()));
    }
    auto impacts = impacts_source_->get_impacts();
    if (!impacts.has_value()) {
        return std::unexpected(std::move(impacts.error()));
    }
    return impacts.value()->get_doc_id_upto(0);
}

VoidResult MaxScoreCache::ensure_cache_size(size_t size) {
    if (cache_.size() < size) {
        cache_.resize(size);
    }
    return {};
}

Result<std::optional<size_t>> MaxScoreCache::get_level(int32_t upto) const {
    auto impacts = impacts_source_->get_impacts();
    if (!impacts.has_value()) {
        return std::unexpected(std::move(impacts.error()));
    }
    const size_t num_levels = impacts.value()->num_levels();
    for (size_t level = 0; level < num_levels; ++level) {
        if (upto <= impacts.value()->get_doc_id_upto(level)) {
            return std::optional<size_t>(level);
        }
    }
    return std::optional<size_t>{};
}

Result<float> MaxScoreCache::get_max_score(int32_t upto) {
    auto level = get_level(upto);
    if (!level.has_value()) {
        return std::unexpected(std::move(level.error()));
    }
    if (!level.value().has_value()) {
        return global_max_score_;
    }
    return get_max_score_with_level(level.value().value());
}

Result<float> MaxScoreCache::get_max_score_with_level_zero() { return get_max_score_with_level(0); }

Result<float> MaxScoreCache::get_max_score_with_level(size_t level) {
    if (auto grown = ensure_cache_size(level + 1); !grown.has_value()) {
        return std::unexpected(std::move(grown.error()));
    }
    auto impacts = impacts_source_->get_impacts();
    if (!impacts.has_value()) {
        return std::unexpected(std::move(impacts.error()));
    }
    const int32_t level_upto = impacts.value()->get_doc_id_upto(level);
    CacheEntry &entry = cache_[level];
    if (entry.upto < level_upto) {
        auto level_impacts = impacts.value()->get_impacts(level);
        if (!level_impacts.has_value()) {
            return std::unexpected(std::move(level_impacts.error()));
        }
        float max_score = 0.0F;
        for (const auto &impact : level_impacts.value()) {
            const float score = scorer_->score(static_cast<float>(impact.freq), impact.norm);
            if (score > max_score) {
                max_score = score;
            }
        }
        entry.score = max_score;
        entry.upto = level_upto;
    }
    return entry.score;
}

Result<std::optional<size_t>> MaxScoreCache::get_skip_level(float min_score) {
    auto impacts = impacts_source_->get_impacts();
    if (!impacts.has_value()) {
        return std::unexpected(std::move(impacts.error()));
    }
    const size_t num_levels = impacts.value()->num_levels();
    std::optional<size_t> skip_level;
    for (size_t level = 0; level < num_levels; ++level) {
        auto max_score = get_max_score_with_level(level);
        if (!max_score.has_value()) {
            return std::unexpected(std::move(max_score.error()));
        }
        if (max_score.value() >= min_score) {
            return skip_level;
        }
        skip_level = level;
    }
    return skip_level;
}

Result<int32_t> MaxScoreCache::get_skip_up_to(float min_score) {
    auto level = get_skip_level(min_score);
    if (!level.has_value()) {
        return std::unexpected(std::move(level.error()));
    }
    if (!level.value().has_value()) {
        return -1;
    }
    auto impacts = impacts_source_->get_impacts();
    if (!impacts.has_value()) {
        return std::unexpected(std::move(impacts.error()));
    }
    return impacts.value()->get_doc_id_upto(level.value().value());
}

} // namespace lucene
