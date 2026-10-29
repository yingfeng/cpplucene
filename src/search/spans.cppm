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

export module lucene_core:spans;

import :doc_id_set_iterator;
import :postings_enum;
import :term;
import :two_phase_iterator;
import :types;

import std;

namespace lucene {

/// Returned by [`Spans::next_start_position`] when the current document has no
/// more positions.
export inline constexpr int32_t NO_MORE_POSITIONS = std::numeric_limits<int32_t>::max();

/// Collects postings information from the leaves of a [`Spans`].
///
/// Port of `SpanCollector` (`queries/span/span_collector.rs`).
export class SpanCollector {
public:
    virtual ~SpanCollector() = default;

    /// Collects the postings at `position`, whose term is `term`.
    [[nodiscard]] virtual VoidResult collect_leaf(PostingsEnum &postings, int32_t position, const Term &term) = 0;

    /// Called when the driving [`Spans`] moved to a new position.
    virtual void reset() = 0;
};

/// Iterates the start/end position pairs of the matches in the current document.
///
/// Port of `Spans` (`queries/span/spans.rs`).  Positions are enumerated by
/// increasing document, then by increasing start position, then by increasing
/// end position.
///
/// **Deviation:** [`do_start_current_doc`] and [`do_current_spans`] are public
/// because the reference implementation's package-private access has no C++
/// equivalent; they are only meant to be called by
/// [`SpanScorer`](lucene_core:span_scorer).
export class Spans : public DocIdSetIterator {
public:
    /// The next start position in the current document, or
    /// [`NO_MORE_POSITIONS`] when the document has no further match.  There is
    /// always at least one start/end pair per matching document.
    [[nodiscard]] virtual Result<int32_t> next_start_position() = 0;

    /// The start position of the current match, `-1` before the first
    /// [`next_start_position`] call on the current document, and
    /// [`NO_MORE_POSITIONS`] after the last one.
    [[nodiscard]] virtual int32_t start_position() const = 0;

    /// The end position of the current match, with the same `-1` /
    /// [`NO_MORE_POSITIONS`] conventions as [`start_position`].
    [[nodiscard]] virtual int32_t end_position() const = 0;

    /// The width of the match, used for the sloppy frequency; lower is better.
    [[nodiscard]] virtual int32_t width() const = 0;

    /// Collects the postings of the current match.
    [[nodiscard]] virtual VoidResult collect(SpanCollector &collector) = 0;

    /// An estimate of the cost of using the positions of this spans for one
    /// document.  Only meaningful when [`as_two_phase_iterator`] is null.
    [[nodiscard]] virtual float positions_cost() const = 0;

    /// A two-phase view of this spans, or a null pointer when the approximation
    /// is the spans itself (a term's spans, where `freq >= 1` already guarantees
    /// a position).
    [[nodiscard]] virtual Result<std::unique_ptr<TwoPhaseIterator>> as_two_phase_iterator();

    /// `Spans(doc=…,start=…,end=…)`; the class name is not part of it, because
    /// C++ has no equivalent of `getClass().getSimpleName()`.
    [[nodiscard]] virtual std::string to_string() const;

    /// Called before the frequency of the current document is computed.
    [[nodiscard]] virtual VoidResult do_start_current_doc() { return {}; }

    /// Called each time the scorer walks to a new position while computing the
    /// frequency.
    [[nodiscard]] virtual VoidResult do_current_spans() { return {}; }
};

} // namespace lucene
