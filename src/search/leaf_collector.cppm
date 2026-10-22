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

export module lucene_core:leaf_collector;

import :types;
import std;

import :doc_id_set_iterator;
import :error;
import :scorable;

namespace lucene {

/// Collects the matches of one leaf (segment).
///
/// Port of `LeafCollector` (`core/search/leaf_collector.rs`).
///
/// **Deviation:** the reference implementation's `collect_stream` / `DocIdStream` bulk API is not
/// ported — no collector in the ported code overrides it, so
/// [`collect`] carries every hit.  The `CompetitiveIterator` associated type
/// became a plain `DocIdSetIterator`.
export class LeafCollector {
public:
    virtual ~LeafCollector() = default;

    /// Called before the first [`collect`]; collectors that need the score save
    /// the pointer and call `score()` themselves.
    [[nodiscard]] virtual VoidResult set_scorer(Scorable & /*scorer*/) { return {}; }

    /// Called once per matching document, in increasing order of `doc`.
    [[nodiscard]] virtual VoidResult collect(int32_t doc, Scorable &scorer) = 0;

    /// An iterator over the documents that may still be competitive, or
    /// `nullptr` when the collector cannot skip anything.
    [[nodiscard]] virtual Result<std::unique_ptr<DocIdSetIterator>> competitive_iterator() {
        return std::unique_ptr<DocIdSetIterator>{};
    }

    /// Called once after the leaf was collected successfully.
    [[nodiscard]] virtual VoidResult finish() { return {}; }

    /// A description used in messages.
    [[nodiscard]] virtual std::string to_string() const { return "LeafCollector"; }
};

} // namespace lucene
