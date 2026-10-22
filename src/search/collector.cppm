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

export module lucene_core:collector;

import :types;

import std;

import :error;
import :leaf_collector;
import :leaf_reader_context;
import :score_mode;

namespace lucene {

export class IndexSearcher;
export class Weight;

/// Gathers the hits of a search.
///
/// Port of the `Collector` trait (`core/search/collector.rs`).
///
/// A collector is asked for one [`LeafCollector`] per leaf.  That leaf
/// collector borrows the collector it came from, so the collector has to
/// outlive it — which is how the search loop uses them (one collector per leaf
/// slice, reduced at the end).
///
/// **Deviations** (C++ mechanics only):
///  * the reference implementation's `get_leaf_collector` is generic over the weight and the
///    searcher; here they are `const Weight *` / `const IndexSearcher &`,
///    because every ported collector ignores the weight (the pointer may be
///    null for the collectors that do not need it);
///  * `set_weight` takes `&mut self` in effect: the port takes it by
///    non-const reference instead of relying on interior mutability.
export class Collector {
public:
    virtual ~Collector() = default;

    /// A collector for the leaf `context`.
    ///
    /// Fails with `collection_terminated` when the leaf holds no document the
    /// collector is interested in; the search loop then carries on with the next
    /// leaf.
    [[nodiscard]] virtual Result<std::unique_ptr<LeafCollector>>
    get_leaf_collector(const LeafReaderContext &context, const Weight *weight, const IndexSearcher &searcher) = 0;

    /// What the collector needs from the scorer.
    [[nodiscard]] virtual ScoreMode score_mode() const = 0;

    /// Tells the collector which weight feeds its leaf collectors.
    [[nodiscard]] virtual VoidResult set_weight(const Weight *weight) {
        (void)weight;
        return {};
    }
};

} // namespace lucene
