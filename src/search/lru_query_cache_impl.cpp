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

module lucene_core:lru_query_cache.impl;

import :lru_query_cache;

import std;

import :constant_score_scorer;
import :doc_id_set_iterator;
import :error;
import :explanation;
import :fixed_bit_set;
import :identity;
import :index_searcher;
import :leaf_reader;
import :leaf_reader_context;
import :query;
import :query_cache;
import :query_caching_policy;
import :scorable;
import :score_mode;
import :scorer;
import :scorer_supplier;
import :types;
import :weight;

namespace lucene {

namespace {

/// The overhead the cache charges for one cached document set, on top of the
/// bit set itself.
constexpr int64_t kDocIdSetOverhead = 64;
/// The overhead the cache charges for one cached query.
constexpr int64_t kEntryOverhead = 64;

/// A [`DocIdSetIterator`] over the set bits of a [`FixedBitSet`].
class FixedBitSetIterator final : public DocIdSetIterator {
public:
    explicit FixedBitSetIterator(std::shared_ptr<const FixedBitSet> bits) : bits_(std::move(bits)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        if (doc_ == NO_MORE_DOCS) {
            return NO_MORE_DOCS;
        }
        const int64_t next = bits_->next_set_bit(static_cast<size_t>(doc_ + 1));
        doc_ = next < 0 ? NO_MORE_DOCS : static_cast<int32_t>(next);
        return doc_;
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        if (target <= doc_) {
            return doc_;
        }
        if (target > static_cast<int32_t>(bits_->length())) {
            doc_ = NO_MORE_DOCS;
            return doc_;
        }
        const int64_t next = bits_->next_set_bit(static_cast<size_t>(target));
        doc_ = next < 0 ? NO_MORE_DOCS : static_cast<int32_t>(next);
        return doc_;
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(bits_->cardinality()); }

private:
    std::shared_ptr<const FixedBitSet> bits_;
    int32_t doc_{-1};
};

/// A [`ScorerSupplier`] over a cached document set: every hit scores `score`.
class CachedScorerSupplier final : public ScorerSupplier {
public:
    CachedScorerSupplier(std::shared_ptr<const FixedBitSet> bits, float score, ScoreMode score_mode)
            : bits_(std::move(bits)), score_(score), score_mode_(score_mode) {}

    [[nodiscard]] Result<std::unique_ptr<Scorer>> get(int64_t /*lead_cost*/, const LeafReaderContext & /*context*/,
                                                      const IndexSearcher & /*searcher*/) override {
        std::unique_ptr<DocIdSetIterator> iterator(new FixedBitSetIterator(bits_));
        return std::unique_ptr<Scorer>(ConstantScoreScorer::from_disi(score_, score_mode_, std::move(iterator)));
    }

    [[nodiscard]] Result<int64_t> cost(const LeafReaderContext & /*context*/,
                                       const IndexSearcher & /*searcher*/) override {
        return static_cast<int64_t>(bits_->cardinality());
    }

private:
    std::shared_ptr<const FixedBitSet> bits_;
    float score_;
    ScoreMode score_mode_;
};

/// The key of one cached query: the reader tree it was cached for, and the query.
struct CacheKey {
    Identity core;
    std::shared_ptr<const Query> query;

    friend bool operator==(const CacheKey &left, const CacheKey &right) {
        if (!(left.core == right.core)) {
            return false;
        }
        if (left.query == right.query) {
            return true;
        }
        return left.query->equals(*right.query);
    }
};

struct CacheKeyHash {
    size_t operator()(const CacheKey &key) const {
        size_t result = key.core.hash();
        result = result * 31 + key.query->hash();
        return result;
    }
};

} // namespace

// ------------------------------------------------------------------- storage

class LRUQueryCache::Impl {
public:
    Impl(int32_t max_size, int64_t max_ram_bytes_used)
            : max_size_(max_size), max_ram_bytes_used_(max_ram_bytes_used) {}

    [[nodiscard]] int64_t doc_id_set_ram_bytes(const FixedBitSet &bits) const {
        return static_cast<int64_t>(FixedBitSet::bits_to_words(bits.length()) * sizeof(uint64_t)) + kDocIdSetOverhead;
    }

    [[nodiscard]] Result<std::shared_ptr<const FixedBitSet>> get(const Identity &core,
                                                                const std::shared_ptr<const Query> &query,
                                                                size_t leaf_ord) {
        auto found = cache_.find(CacheKey{core, query});
        if (found == cache_.end()) {
            return std::shared_ptr<const FixedBitSet>{};
        }
        auto &entry = found->second;
        if (leaf_ord >= entry.leaves.size() || entry.leaves[leaf_ord] == nullptr) {
            return std::shared_ptr<const FixedBitSet>{};
        }
        ++hit_count_;
        touch(found);
        return entry.leaves[leaf_ord];
    }

    void put(const Identity &core, std::shared_ptr<const Query> query, size_t leaf_ord,
             std::shared_ptr<const FixedBitSet> bits) {
        CacheKey key{core, std::move(query)};
        auto found = cache_.find(key);
        if (found == cache_.end()) {
            Entry entry;
            entry.leaves.resize(leaf_ord + 1);
            entry.leaves[leaf_ord] = std::move(bits);
            entry.ram_bytes = kEntryOverhead + doc_id_set_ram_bytes(*entry.leaves[leaf_ord]);
            ram_bytes_used_ += entry.ram_bytes;
            ++cache_count_;
            lru_.push_front(key);
            entry.lru_position = lru_.begin();
            cache_.emplace(std::move(key), std::move(entry));
        } else {
            auto &entry = found->second;
            if (leaf_ord < entry.leaves.size() && entry.leaves[leaf_ord] != nullptr) {
                // Another search cached this leaf in the meantime.
                return;
            }
            if (leaf_ord >= entry.leaves.size()) {
                entry.leaves.resize(leaf_ord + 1);
            }
            entry.leaves[leaf_ord] = std::move(bits);
            const int64_t added = doc_id_set_ram_bytes(*entry.leaves[leaf_ord]);
            entry.ram_bytes += added;
            ram_bytes_used_ += added;
            ++cache_count_;
            touch(found);
        }
        evict();
    }

    void clear() {
        cache_.clear();
        lru_.clear();
        ram_bytes_used_ = 0;
    }

    [[nodiscard]] int64_t cache_size() const { return static_cast<int64_t>(cache_.size()); }

    [[nodiscard]] int64_t hit_count() const { return hit_count_; }
    [[nodiscard]] int64_t miss_count() const { return miss_count_; }
    [[nodiscard]] int64_t cache_count() const { return cache_count_; }
    [[nodiscard]] int64_t eviction_count() const { return eviction_count_; }
    [[nodiscard]] int64_t ram_bytes_used() const { return ram_bytes_used_; }

    void record_miss() { ++miss_count_; }

private:
    struct Entry {
        /// The cached document sets, indexed by leaf ord; a null pointer means
        /// "not cached yet".
        std::vector<std::shared_ptr<const FixedBitSet>> leaves;
        int64_t ram_bytes{0};
        /// Where this entry sits in the LRU list.
        std::list<CacheKey>::iterator lru_position;
    };

    using CacheMap = std::unordered_map<CacheKey, Entry, CacheKeyHash>;

    void touch(typename CacheMap::iterator found) {
        lru_.splice(lru_.begin(), lru_, found->second.lru_position);
    }

    /// Drops the least recently used entries until the cache fits its limits.
    void evict() {
        while ((max_size_ >= 0 && static_cast<int32_t>(cache_.size()) > max_size_) ||
               (max_ram_bytes_used_ >= 0 && ram_bytes_used_ > max_ram_bytes_used_)) {
            if (cache_.size() <= 1) {
                // Never evict the entry that was just cached.
                return;
            }
            const CacheKey &victim = lru_.back();
            auto found = cache_.find(victim);
            if (found == cache_.end()) {
                lru_.pop_back();
                continue;
            }
            for (const auto &leaf : found->second.leaves) {
                if (leaf != nullptr) {
                    ++eviction_count_;
                }
            }
            ram_bytes_used_ -= found->second.ram_bytes;
            if (ram_bytes_used_ < 0) {
                ram_bytes_used_ = 0;
            }
            cache_.erase(found);
            lru_.pop_back();
        }
    }

    int32_t max_size_;
    int64_t max_ram_bytes_used_;
    int64_t hit_count_{0};
    int64_t miss_count_{0};
    int64_t cache_count_{0};
    int64_t eviction_count_{0};
    int64_t ram_bytes_used_{0};
    std::list<CacheKey> lru_;
    CacheMap cache_;
};

// --------------------------------------------------------- caching wrapper

namespace {

/// A [`Weight`] that answers the documents of its leaf from the cache when it
/// can, and fills the cache otherwise.
///
/// Port of `CachingWrapperWeight` (`core/search/lru_query_cache.rs`).
class CachingWrapperWeight final : public Weight {
public:
    CachingWrapperWeight(std::shared_ptr<const Query> query, std::unique_ptr<Weight> inner,
                         std::shared_ptr<LRUQueryCache> cache, std::shared_ptr<const QueryCachingPolicy> policy)
            : query_(std::move(query)), inner_(std::move(inner)), cache_(std::move(cache)), policy_(std::move(policy)) {}

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                              const IndexSearcher &searcher) const override {
        return inner_->explain(context, doc, searcher);
    }

    [[nodiscard]] const Query &get_query() const override { return *query_; }

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext & /*context*/) const override {
        // A weight that already consults the cache must not be cached again.
        return false;
    }

    [[nodiscard]] Result<std::unique_ptr<Scorer>> scorer(const LeafReaderContext &context,
                                                         const IndexSearcher &searcher) override {
        auto supplier = scorer_supplier(context, searcher);
        if (!supplier.has_value()) {
            return std::unexpected(std::move(supplier.error()));
        }
        if (supplier.value() == nullptr) {
            return std::unique_ptr<Scorer>{};
        }
        return supplier.value()->get(1, context, searcher);
    }

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext &context,
                                                                         const IndexSearcher &searcher) override {
        const size_t leaf_ord = static_cast<size_t>(context.ord());
        auto cached = cache_->get_cached_doc_ids(context.top_parent().id, query_, leaf_ord);
        if (!cached.has_value()) {
            return std::unexpected(std::move(cached.error()));
        }
        if (cached.value() != nullptr) {
            // Cached document sets carry no scores: the cache may only be used
            // when the search does not need them.
            return std::unique_ptr<ScorerSupplier>(new CachedScorerSupplier(cached.value(), 0.0F,
                                                                           ScoreMode::CompleteNoScores));
        }
        if (!should_cache(context)) {
            return inner_->scorer_supplier(context, searcher);
        }

        // Miss: compute the documents once, cache them, and answer them.
        auto bits = compute_doc_ids(context, searcher);
        if (!bits.has_value()) {
            return std::unexpected(std::move(bits.error()));
        }
        cache_->record_miss();
        if (bits.value() != nullptr) {
            cache_->put_cached_doc_ids(context.top_parent().id, query_, leaf_ord, bits.value());
            return std::unique_ptr<ScorerSupplier>(
                    new CachedScorerSupplier(bits.value(), 0.0F, ScoreMode::CompleteNoScores));
        }
        return inner_->scorer_supplier(context, searcher);
    }

private:
    /// Whether the leaf is worth caching: the policy has to agree, and the leaf
    /// itself must be cacheable.
    [[nodiscard]] bool should_cache(const LeafReaderContext &context) {
        auto cacheable = inner_->is_cacheable(context);
        if (!cacheable.has_value() || !cacheable.value()) {
            return false;
        }
        auto wanted = policy_->should_cache(*query_);
        return wanted.has_value() && wanted.value();
    }

    /// Iterates the wrapped scorer and records every confirmed document.
    [[nodiscard]] Result<std::shared_ptr<const FixedBitSet>> compute_doc_ids(const LeafReaderContext &context,
                                                                             const IndexSearcher &searcher) {
        auto max_doc = context.reader()->max_doc();
        if (!max_doc.has_value()) {
            return std::unexpected(std::move(max_doc.error()));
        }
        auto scorer = inner_->scorer(context, searcher);
        if (!scorer.has_value()) {
            return std::unexpected(std::move(scorer.error()));
        }
        if (scorer.value() == nullptr) {
            // Nothing matches this leaf.  The empty set is cached as well, so an
            // expensive query with no hit is not run again.
            return std::shared_ptr<const FixedBitSet>(
                    std::make_shared<FixedBitSet>(static_cast<size_t>(max_doc.value())));
        }
        auto bits = std::make_shared<FixedBitSet>(static_cast<size_t>(max_doc.value()));
        const bool two_phase = scorer.value()->has_two_phase_iterator() == TwoPhaseState::Yes;
        int32_t doc = scorer.value()->doc_id();
        if (doc == -1) {
            auto next = scorer.value()->next_doc();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
            doc = next.value();
        }
        while (doc != NO_MORE_DOCS) {
            bool matches = true;
            if (two_phase) {
                auto confirmed = scorer.value()->matches();
                if (!confirmed.has_value()) {
                    return std::unexpected(std::move(confirmed.error()));
                }
                matches = confirmed.value();
            }
            if (matches) {
                bits->set(static_cast<size_t>(doc));
            }
            auto next = scorer.value()->next_doc();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
            doc = next.value();
        }
        return std::shared_ptr<const FixedBitSet>(std::move(bits));
    }

    std::shared_ptr<const Query> query_;
    std::unique_ptr<Weight> inner_;
    std::shared_ptr<LRUQueryCache> cache_;
    std::shared_ptr<const QueryCachingPolicy> policy_;
};

} // namespace

// ------------------------------------------------------------- LRUQueryCache

LRUQueryCache::LRUQueryCache(int32_t max_size, int64_t max_ram_bytes_used)
        : impl_(std::make_unique<Impl>(max_size, max_ram_bytes_used)) {}

LRUQueryCache::~LRUQueryCache() = default;

Result<std::shared_ptr<LRUQueryCache>> LRUQueryCache::create(int32_t max_size, int64_t max_ram_bytes_used) {
    if (max_size <= 0) {
        return err(LuceneError::illegal_argument("max_size must be greater than 0, got " + std::to_string(max_size)));
    }
    if (max_ram_bytes_used <= 0) {
        return err(LuceneError::illegal_argument("max_ram_bytes_used must be greater than 0, got " +
                                                 std::to_string(max_ram_bytes_used)));
    }
    // `make_shared` cannot reach the private constructor.
    std::shared_ptr<LRUQueryCache> cache(new LRUQueryCache(max_size, max_ram_bytes_used));
    return cache;
}

Result<std::shared_ptr<LRUQueryCache>> LRUQueryCache::with_max_size(int32_t max_size) {
    if (max_size <= 0) {
        return err(LuceneError::illegal_argument("max_size must be greater than 0, got " + std::to_string(max_size)));
    }
    std::shared_ptr<LRUQueryCache> cache(new LRUQueryCache(max_size, std::numeric_limits<int64_t>::max()));
    return cache;
}

Result<std::unique_ptr<Weight>> LRUQueryCache::do_cache(std::shared_ptr<const Query> query,
                                                      std::unique_ptr<Weight> weight,
                                                      std::shared_ptr<const QueryCachingPolicy> policy) {
    // The policy has to see every use, so that it can decide when a query
    // becomes worth caching.
    policy->on_use(*query);
    // Keep the cache alive as long as the weight it created.
    std::shared_ptr<LRUQueryCache> self = shared_from_this();
    std::unique_ptr<Weight> wrapper(
            new CachingWrapperWeight(std::move(query), std::move(weight), std::move(self), std::move(policy)));
    return wrapper;
}

int64_t LRUQueryCache::hit_count() const noexcept { return impl_->hit_count(); }
int64_t LRUQueryCache::miss_count() const noexcept { return impl_->miss_count(); }
int64_t LRUQueryCache::cache_size() const noexcept { return impl_->cache_size(); }
int64_t LRUQueryCache::cache_count() const noexcept { return impl_->cache_count(); }
int64_t LRUQueryCache::eviction_count() const noexcept { return impl_->eviction_count(); }
int64_t LRUQueryCache::ram_bytes_used() const noexcept { return impl_->ram_bytes_used(); }

void LRUQueryCache::clear() { impl_->clear(); }

Result<std::shared_ptr<const FixedBitSet>>
LRUQueryCache::get_cached_doc_ids(const Identity &core, const std::shared_ptr<const Query> &query, size_t leaf_ord) {
    return impl_->get(core, query, leaf_ord);
}

void LRUQueryCache::put_cached_doc_ids(const Identity &core, std::shared_ptr<const Query> query, size_t leaf_ord,
                                       std::shared_ptr<const FixedBitSet> bits) {
    impl_->put(core, std::move(query), leaf_ord, std::move(bits));
}

void LRUQueryCache::record_miss() noexcept { impl_->record_miss(); }

} // namespace lucene
