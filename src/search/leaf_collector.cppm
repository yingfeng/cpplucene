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

import :doc_id_stream;
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
/// **Deviation:** the reference implementation's windowed boolean scorer (which
/// produces the streams this API batches) is not ported, so
/// [`collect_stream`] exists and is honoured (the hit-count collector overrides
/// it), but the ported bulk scorers still hand every hit to [`collect`].  The
/// `CompetitiveIterator` associated type became a plain `DocIdSetIterator`.
export class LeafCollector {
public:
    virtual ~LeafCollector() = default;

    /// Called before the first [`collect`]; collectors that need the score save
    /// the pointer and call `score()` themselves.
    [[nodiscard]] virtual VoidResult set_scorer(Scorable & /*scorer*/) { return {}; }

    /// Called once per matching document, in increasing order of `doc`.
    [[nodiscard]] virtual VoidResult collect(int32_t doc, Scorable &scorer) = 0;

    /// Whether [`collect_with_score`] is implemented, i.e. whether the caller may
    /// compute the score itself and hand it in.
    ///
    /// **Deviation:** the reference implementation monomorphises the bulk scorer
    /// over the collector, so the score is computed in the loop by construction.
    /// This port keeps the collector abstract, so a bulk scorer asks first and
    /// then calls [`collect_with_score`] on the hot path — which is what lets the
    /// score be computed by a *statically dispatched* scorer instead of through
    /// [`Scorable::score`].
    [[nodiscard]] virtual bool supports_precomputed_score() const { return false; }

    /// The same as [`collect`], for a caller that already computed `score`.
    /// Only called when [`supports_precomputed_score`] is true.
    [[nodiscard]] virtual Result<bool> collect_with_score(int32_t /*doc*/, Scorable & /*scorable*/, float /*score*/) {
        return err(LuceneError::need_implemented("collect_with_score"));
    }

    /// Bulk-collects the documents of `stream`.
    ///
    /// The default implementation hands every document to
    /// [`collect`](Self::collect); collectors that can count or group documents
    /// without per-hit work override it.
    [[nodiscard]] virtual VoidResult collect_stream(DocIdStream &stream, Scorable &scorer);

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
