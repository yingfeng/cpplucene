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

export module lucene_core:query_cache;

import :types;
import std;

import :error;

namespace lucene {

export class Query;
export class QueryCachingPolicy;
export class Weight;

/// A cache for the documents that a query matches.
///
/// Port of the `QueryCache` trait (`core/search/query_cache.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * the reference implementation's `QueryCacheEnum` (LRU or custom) collapses
///    into this abstract class, which is already type-erased;
///  * the policy is passed per call as a shared pointer instead of an `Arc` of an
///    enum, and the weight is handed over instead of being cloned, since a `Weight`
///    is move-only here.
export class QueryCache {
public:
    virtual ~QueryCache() = default;

    /// Wraps `weight` so that the documents it matches are cached per leaf,
    /// according to `policy`.
    ///
    /// The returned weight is only equivalent when scores are not needed.
    [[nodiscard]] virtual Result<std::unique_ptr<Weight>>
    do_cache(std::shared_ptr<const Query> query, std::unique_ptr<Weight> weight,
             std::shared_ptr<const QueryCachingPolicy> policy) = 0;
};

} // namespace lucene
