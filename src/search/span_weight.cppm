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

export module lucene_core:span_weight;

import :index_searcher;
import :leaf_reader_context;
import :query;
import :scorer;
import :scorer_supplier;
import :similarity;
import :span_query;
import :spans;
import :term;
import :term_states;
import :weight;

import std;

namespace lucene {

/// Orders terms by field and then by bytes, which is what [`Term::compare_to`]
/// does and what `std::map` needs.
export struct TermLess {
    [[nodiscard]] bool operator()(const Term &left, const Term &right) const { return left.compare_to(right) < 0; }
};

/// The term states a span weight was built from, keyed by term.
export using TermStatesMap = std::map<Term, std::shared_ptr<TermStates>, TermLess>;

/// What a span query needs from the postings of a leaf.
export enum class SpanPostings : uint8_t {
    /// Positions only.
    kPositions,
    /// Positions and payloads.
    kPayloads,
    /// Positions, payloads and offsets.
    kOffsets,
};

/// The weight of a span query: it owns the similarity scorer and asks its
/// subclass for the [`Spans`] of a leaf.
///
/// Port of `SpanWeight` (`queries/span/span_weight.rs`).
export class SpanWeight : public Weight {
public:
    /// `term_states` holds the states of every term the query uses; pass an empty
    /// map when the search does not need scores.
    SpanWeight(const SpanQuery &query, const IndexSearcher &searcher, TermStatesMap term_states, float boost);

    [[nodiscard]] const Query &get_query() const override { return *query_; }

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext &context,
                                                                          const IndexSearcher &searcher) override;

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                              const IndexSearcher &searcher) const override;

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext &context) const override;

    /// Adds the states of the terms this weight uses to `contexts`.
    [[nodiscard]] virtual VoidResult extract_term_states(TermStatesMap &contexts) const = 0;

    /// The spans of `context`, or a null pointer when no document of the leaf can
    /// match.
    [[nodiscard]] virtual Result<std::unique_ptr<Spans>> get_spans(const LeafReaderContext &context,
                                                                  SpanPostings required) = 0;

    [[nodiscard]] std::shared_ptr<SimScorer> sim_scorer() const noexcept { return sim_scorer_; }
    [[nodiscard]] const Similarity &similarity() const noexcept { return *similarity_; }
    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    [[nodiscard]] float boost() const noexcept { return boost_; }

protected:
    const SpanQuery *query_;
    const Similarity *similarity_;
    std::shared_ptr<SimScorer> sim_scorer_;
    std::string field_;
    float boost_;
};

} // namespace lucene
