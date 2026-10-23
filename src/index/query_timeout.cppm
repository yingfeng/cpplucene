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

export module lucene_core:query_timeout;

import :types;

namespace lucene {

/// Tells a long running search when its time budget is spent.
///
/// Port of the `QueryTimeout` trait (`core/index/query_timeout.rs`).
///
/// A [`TimeLimitingBulkScorer`](:time_limiting_bulk_scorer) asks this before each
/// block of documents; it must be cheap and thread safe.
export class QueryTimeout {
public:
    virtual ~QueryTimeout() = default;

    /// `true` when the search has to stop, which surfaces as
    /// `kTimeExceeded`.
    [[nodiscard]] virtual bool should_exit() const = 0;
};

} // namespace lucene
