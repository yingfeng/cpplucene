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

export module lucene_core:query_caching_policy;

import :types;
import std;

import :error;
import :frequency_tracking_ring_buffer;
import :query;

namespace lucene {

/// Decides which queries a [`QueryCache`](lucene_core:query_cache) should cache.
///
/// Port of the `QueryCachingPolicy` trait
/// (`core/search/query_caching_policy.rs`).  Implementations must be
/// thread-safe.
export class QueryCachingPolicy {
public:
    virtual ~QueryCachingPolicy() = default;

    /// Called every time a cached query is used, so a policy can track usage
    /// statistics.
    virtual void on_use(const Query &query) const = 0;

    /// Whether `query` is worth caching.  Called when the query is not cached
    /// yet: a `true` answer makes the cache build an entry.
    [[nodiscard]] virtual Result<bool> should_cache(const Query &query) const = 0;
};

/// A policy that caches a query once it has been seen often enough.
///
/// Port of `UsageTrackingQueryCachingPolicy`
/// (`core/search/usage_tracking_query_caching_policy.rs`).
///
/// Only the hash codes of the recent queries are tracked, so that a large query
/// is never kept alive by the policy; a hash collision at worst caches a query
/// that was not in fact used often enough.
export class UsageTrackingQueryCachingPolicy final : public QueryCachingPolicy {
public:
    /// A policy that remembers the last 256 queries.
    [[nodiscard]] static Result<std::unique_ptr<UsageTrackingQueryCachingPolicy>> create();

    /// A policy that remembers the last `history_size` queries.
    [[nodiscard]] static Result<std::unique_ptr<UsageTrackingQueryCachingPolicy>> with_history_size(size_t history_size);

    void on_use(const Query &query) const override;

    [[nodiscard]] Result<bool> should_cache(const Query &query) const override;

    /// How often `query` was used recently.
    [[nodiscard]] int32_t frequency(const Query &query) const;

    /// The number of uses after which `query` becomes worth caching: 2 for a
    /// costly query, 5 otherwise, and 4 for a compound query.
    [[nodiscard]] static int32_t min_frequency_to_cache(const Query &query);

    /// Whether `query` is expensive enough to be cached early.
    [[nodiscard]] static bool is_costly(const Query &query);

private:
    explicit UsageTrackingQueryCachingPolicy(FrequencyTrackingRingBuffer buffer);

    /// The hash code every query is tracked under, with the sentinel value
    /// (`INT32_MAX`) reserved for "no query".
    [[nodiscard]] static int32_t hash_code(const Query &query);

    /// Whether the policy refuses to cache `query` at all, because it is already
    /// fast or cannot match anything.
    [[nodiscard]] static bool should_never_cache(const Query &query);

    mutable FrequencyTrackingRingBuffer recently_used_filters_;
};

} // namespace lucene
