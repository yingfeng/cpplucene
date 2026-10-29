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

export module lucene_core:span_contain_query;

import :conjunction_spans;
import :index_searcher;
import :leaf_reader_context;
import :query;
import :query_visitor;
import :score_mode;
import :span_query;
import :span_weight;
import :spans;
import :types;
import :weight;

import std;

namespace lucene {

export class SpanContainQuery;

/// The spans of a span contain query: both operands must match the document, and
/// the reported match is the one of the operand named `lead`.
///
/// Port of `ContainSpans` (`queries/span/contain_spans.rs`).  The two operands
/// are held in the conjunction order (`big` first), so the accepted span is
/// `sub_spans()[lead]`.
export class ContainSpans : public ConjunctionSpans {
public:
    /// `lead` is 0 for the containing variant and 1 for the within variant.
    ContainSpans(std::vector<std::unique_ptr<Spans>> sub_spans, size_t lead, bool containing);

    [[nodiscard]] Result<int32_t> next_start_position() override;
    [[nodiscard]] int32_t start_position() const override;
    [[nodiscard]] int32_t end_position() const override;
    [[nodiscard]] int32_t width() const override { return lead().width(); }

    [[nodiscard]] VoidResult collect(SpanCollector &collector) override;

protected:
    [[nodiscard]] Result<bool> two_phase_current_doc_matches() override;

    [[nodiscard]] Spans &lead() const { return *sub_spans_[lead_]; }
    [[nodiscard]] Spans &big() const { return *sub_spans_[0]; }
    [[nodiscard]] Spans &little() const { return *sub_spans_[1]; }

private:
    size_t lead_;
    bool containing_;
};

/// The weight of a [`SpanContainQuery`]: the conjunction of the two operands'
/// spans, filtered down to the contained/containing pairs.
///
/// Port of `SpanContainQuery.SpanContainWeight`.
export class SpanContainWeight : public SpanWeight {
public:
    SpanContainWeight(const SpanContainQuery &query, std::unique_ptr<SpanWeight> big_weight,
                      std::unique_ptr<SpanWeight> little_weight, const IndexSearcher &searcher,
                      TermStatesMap term_states, float boost);

    [[nodiscard]] VoidResult extract_term_states(TermStatesMap &contexts) const override;

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext &context) const override;

protected:
    /// The two operands' spans of `context`, or an empty vector when one of them
    /// has none (so nothing can match in this leaf).
    [[nodiscard]] Result<std::vector<std::unique_ptr<Spans>>> prepare_conjunction(const LeafReaderContext &context,
                                                                                  SpanPostings required);

    std::unique_ptr<SpanWeight> big_weight_;
    std::unique_ptr<SpanWeight> little_weight_;
};

/// The base of `SpanContainingQuery` and `SpanWithinQuery`: both operands have to
/// match the same document, one containing the other.
///
/// Port of `SpanContainQuery` (`queries/span/span_contain_query.rs`).
///
/// **Deviation:** the reference implementation rewrites by cloning and swapping
/// the operands; C++ has no `Cloneable`, so every subclass answers
/// [`with_parts`](SpanContainQuery::with_parts).
export class SpanContainQuery : public SpanQuery {
public:
    [[nodiscard]] const std::shared_ptr<SpanQuery> &big() const noexcept { return big_; }
    [[nodiscard]] const std::shared_ptr<SpanQuery> &little() const noexcept { return little_; }

    [[nodiscard]] std::string_view get_field() const override { return big_->get_field(); }

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                               float boost) const override;

    [[nodiscard]] Result<std::shared_ptr<Query>> rewrite(const IndexSearcher &searcher) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

protected:
    SpanContainQuery(std::shared_ptr<SpanQuery> big, std::shared_ptr<SpanQuery> little, uint32_t class_hash);

    /// A copy of this query with the given operands; used by [`rewrite`].
    [[nodiscard]] virtual std::shared_ptr<SpanQuery> with_parts(std::shared_ptr<SpanQuery> big,
                                                                std::shared_ptr<SpanQuery> little) const = 0;

    /// Builds the subclass' weight from the two operand weights.
    [[nodiscard]] virtual std::unique_ptr<SpanWeight> build_weight(std::unique_ptr<SpanWeight> big_weight,
                                                                   std::unique_ptr<SpanWeight> little_weight,
                                                                   const IndexSearcher &searcher, TermStatesMap term_states,
                                                                   float boost) const = 0;

    [[nodiscard]] Result<std::string> to_string_with_name(std::string_view field, std::string_view name) const;

    std::shared_ptr<SpanQuery> big_;
    std::shared_ptr<SpanQuery> little_;
    uint32_t class_hash_;
};

} // namespace lucene
