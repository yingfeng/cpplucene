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

export module lucene_core:similarity;

import :types;
import std;

import :collection_statistics;
import :error;
import :explanation;
import :field_invert_state;
import :term_statistics;

namespace lucene {

/// Scores a single document of a query term.
///
/// Port of the `SimScorer` trait (`core/search/similarities_impl/similarities.rs`).
export class SimScorer {
public:
    virtual ~SimScorer() = default;

    /// The score of a document with `freq` occurrences of the term and the
    /// encoded norm `norm` (never `0`; use `1` when norms are disabled).
    ///
    /// Scores must be monotonic: non-decreasing in `freq`, non-increasing in the
    /// unsigned `norm`.
    [[nodiscard]] virtual float score(float freq, int64_t norm) const = 0;

    /// Explains [`score`]; the default implementation reports the plain value.
    [[nodiscard]] virtual Result<Explanation> explain(const Explanation &freq, int64_t norm) const;
};

/// The components of Lucene scoring.
///
/// Port of the `Similarity` trait (`core/search/similarities_impl/similarities.rs`).
///
/// Used at index time ([`compute_norm`]) and at query time ([`scorer`], which
/// turns collection-level statistics into a [`SimScorer`]).
export class Similarity {
public:
    virtual ~Similarity() = default;

    /// Whether overlap tokens (position increment `0`) are discounted from the
    /// document length.
    [[nodiscard]] virtual bool get_discount_overlaps() const { return true; }

    /// Computes the norm byte of a field at index time.
    ///
    /// The default encodes the field length with `SmallFloat::int_to_byte4`, as
    /// Lucene's supplied similarities do, and sign extends it to `int64_t`.
    [[nodiscard]] virtual Result<int64_t> compute_norm(const FieldInvertState &state) const;

    /// Computes the collection-level weight of the query from the statistics of
    /// `boost`, the field and its terms.
    [[nodiscard]] virtual Result<std::unique_ptr<SimScorer>> scorer(
            float boost, const CollectionStatistics &collection_stats,
            std::span<const TermStatistics> term_stats) const = 0;

    /// A human readable description, used in `toString` of the queries that
    /// reference it.
    [[nodiscard]] virtual std::string name() const = 0;
};

} // namespace lucene
