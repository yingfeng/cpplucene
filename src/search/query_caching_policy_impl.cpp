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

module lucene_core:query_caching_policy.impl;

import :query_caching_policy;

import std;

import :boolean_query;
import :constant_score_query;
import :disjunction_max_query;
import :error;
import :field_exists_query;
import :frequency_tracking_ring_buffer;
import :match_all_docs_query;
import :match_no_docs_query;
import :multi_term_query;
import :point_range_query;
import :query;
import :term_query;
import :types;

namespace lucene {

namespace {

/// The hash code that the policy uses as a sentinel in the ring buffer.
constexpr int32_t kSentinel = std::numeric_limits<int32_t>::max();

} // namespace

UsageTrackingQueryCachingPolicy::UsageTrackingQueryCachingPolicy(FrequencyTrackingRingBuffer buffer)
        : recently_used_filters_(std::move(buffer)) {}

Result<std::unique_ptr<UsageTrackingQueryCachingPolicy>> UsageTrackingQueryCachingPolicy::create() {
    return with_history_size(256);
}

Result<std::unique_ptr<UsageTrackingQueryCachingPolicy>>
UsageTrackingQueryCachingPolicy::with_history_size(size_t history_size) {
    auto buffer = FrequencyTrackingRingBuffer::create(history_size, kSentinel);
    if (!buffer.has_value()) {
        return std::unexpected(std::move(buffer.error()));
    }
    std::unique_ptr<UsageTrackingQueryCachingPolicy> policy(
            new UsageTrackingQueryCachingPolicy(std::move(buffer.value())));
    return policy;
}

int32_t UsageTrackingQueryCachingPolicy::hash_code(const Query &query) {
    return static_cast<int32_t>(query.hash() & 0x7FFFFFFFULL);
}

bool UsageTrackingQueryCachingPolicy::should_never_cache(const Query &query) {
    if (dynamic_cast<const TermQuery *>(&query) != nullptr) {
        // Term queries are already fast enough.
        return true;
    }
    if (dynamic_cast<const FieldExistsQuery *>(&query) != nullptr) {
        return true;
    }
    if (dynamic_cast<const MatchAllDocsQuery *>(&query) != nullptr) {
        // Its iterator is faster than a bit set.
        return true;
    }
    if (dynamic_cast<const MatchNoDocsQuery *>(&query) != nullptr) {
        // It is cheap to notice that it cannot match anything.
        return true;
    }
    if (const auto *boolean = dynamic_cast<const BooleanQuery *>(&query); boolean != nullptr) {
        return boolean->clauses().empty();
    }
    if (const auto *disjunction = dynamic_cast<const DisjunctionMaxQuery *>(&query); disjunction != nullptr) {
        return disjunction->disjuncts().empty();
    }
    return false;
}

bool UsageTrackingQueryCachingPolicy::is_costly(const Query &query) {
    if (dynamic_cast<const MultiTermQuery *>(&query) != nullptr) {
        return true;
    }
    if (dynamic_cast<const PointRangeQuery *>(&query) != nullptr) {
        return true;
    }
    if (const auto *constant_score = dynamic_cast<const ConstantScoreQuery *>(&query); constant_score != nullptr) {
        // The constant-score wrappers of a multi-term query expand a whole term
        // dictionary, which is as costly as the multi-term query itself.
        return dynamic_cast<const MultiTermQuery *>(constant_score->query().get()) != nullptr ||
               dynamic_cast<const MultiTermQueryConstantScoreWrapper *>(constant_score->query().get()) != nullptr;
    }
    return false;
}

int32_t UsageTrackingQueryCachingPolicy::min_frequency_to_cache(const Query &query) {
    if (is_costly(query)) {
        return 2;
    }
    int32_t min_frequency = 5;
    if (dynamic_cast<const BooleanQuery *>(&query) != nullptr ||
        dynamic_cast<const DisjunctionMaxQuery *>(&query) != nullptr) {
        // Reusing "A OR B" without ever using "A" and "B" alone would otherwise
        // cache all three queries, which is wasteful.
        --min_frequency;
    }
    return min_frequency;
}

int32_t UsageTrackingQueryCachingPolicy::frequency(const Query &query) const {
    return recently_used_filters_.frequency(hash_code(query));
}

void UsageTrackingQueryCachingPolicy::on_use(const Query &query) const {
    if (should_never_cache(query)) {
        return;
    }
    recently_used_filters_.add(hash_code(query));
}

Result<bool> UsageTrackingQueryCachingPolicy::should_cache(const Query &query) const {
    if (should_never_cache(query)) {
        return false;
    }
    return frequency(query) >= min_frequency_to_cache(query);
}

} // namespace lucene
