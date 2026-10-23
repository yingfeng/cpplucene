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

export module lucene_core:lru_query_cache;

import :types;
import std;

import :error;
import :fixed_bit_set;
import :identity;
import :query;
import :query_cache;
import :query_caching_policy;

namespace lucene {

export class Weight;

/// A [`QueryCache`] that evicts queries with an LRU (least-recently-used) policy,
/// to stay under a maximum number of entries and a maximum number of bytes.
///
/// Port of `LRUQueryCache` (`core/search/lru_query_cache.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * the reference implementation's `Inner` with its read-write lock, atomic
///    counters and test hooks collapses into an implementation object: the port
///    is single-threaded and has no `Accountable`/hook machinery;
///  * cached documents are kept as one [`FixedBitSet`] per leaf of one reader
///    (the reference implementation can keep any `DocIdSet`, e.g. a roaring
///    bitmap);
///  * `clear_query` / `clear_core_cache_key` are not ported; [`clear`] drops
///    everything.
export class LRUQueryCache final : public QueryCache, public std::enable_shared_from_this<LRUQueryCache> {
public:
    /// A cache of at most `max_size` entries and `max_ram_bytes_used` bytes.
    [[nodiscard]] static Result<std::shared_ptr<LRUQueryCache>> create(int32_t max_size, int64_t max_ram_bytes_used);

    /// Same, without a byte limit.
    [[nodiscard]] static Result<std::shared_ptr<LRUQueryCache>> with_max_size(int32_t max_size);

    // ---- QueryCache ----

    [[nodiscard]] Result<std::unique_ptr<Weight>> do_cache(std::shared_ptr<const Query> query,
                                                          std::unique_ptr<Weight> weight,
                                                          std::shared_ptr<const QueryCachingPolicy> policy) override;

    // ---- statistics ----

    /// How often a cached document set was used.
    [[nodiscard]] int64_t hit_count() const noexcept;
    /// How often the cache had to compute a document set.
    [[nodiscard]] int64_t miss_count() const noexcept;
    /// The number of cached queries.
    [[nodiscard]] int64_t cache_size() const noexcept;
    /// The number of document sets that were ever cached.
    [[nodiscard]] int64_t cache_count() const noexcept;
    /// The number of evicted document sets.
    [[nodiscard]] int64_t eviction_count() const noexcept;
    /// The number of bytes the cached document sets take.
    [[nodiscard]] int64_t ram_bytes_used() const noexcept;

    /// Drops every entry.
    void clear();

    // ---- internals used by the caching weight ----

    /// The cached documents of `query` in the leaf `leaf_ord` of the reader
    /// `core`, or a null pointer; a hit is counted when there is one.
    [[nodiscard]] Result<std::shared_ptr<const FixedBitSet>>
    get_cached_doc_ids(const Identity &core, const std::shared_ptr<const Query> &query, size_t leaf_ord);

    /// Caches `bits` for `query` in the leaf `leaf_ord` of the reader `core`,
    /// evicting the least recently used entries when the cache is full.
    void put_cached_doc_ids(const Identity &core, std::shared_ptr<const Query> query, size_t leaf_ord,
                            std::shared_ptr<const FixedBitSet> bits);

    /// Counts a document set that had to be computed without being cached.
    void record_miss() noexcept;

    ~LRUQueryCache() override;

private:
    LRUQueryCache(int32_t max_size, int64_t max_ram_bytes_used);

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lucene
