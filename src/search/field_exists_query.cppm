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

export module lucene_core:field_exists_query;

import :types;
import std;

import :error;
import :explanation;
import :index_searcher;
import :leaf_reader;
import :leaf_reader_context;
import :query;
import :query_visitor;
import :score_mode;
import :scorer;
import :scorer_supplier;
import :weight;

namespace lucene {

export class FieldExistsQuery;

/// Matches the documents that have a value for a field.
///
/// Port of `FieldExistsQuery` (`core/search/field_exists_query.rs`).
///
/// A document counts as having a value when the field indexes norms, doc values
/// or vectors; a field that indexes none of them is a mistake and reports
/// `kIllegalArgument`.  The query scores every match the same, like a constant
/// score query.
///
/// **Deviation:** the vector branch is not ported (the port has no vector
/// values), and a field that only indexes vectors therefore reports
/// `kUnsupportedOperation`.
export class FieldExistsQuery final : public Query {
public:
    /// A query for the documents that have a value for `field`.
    explicit FieldExistsQuery(std::string field) : field_(std::move(field)) {}

    /// The field the query reads.
    [[nodiscard]] const std::string &field() const noexcept { return field_; }

    // ---- Query ----

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    /// `FieldExistsQuery [field=<field>]`, the form the reference implementation
    /// prints.
    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    std::string field_;
};

/// The weight of a [`FieldExistsQuery`]: per leaf it iterates whatever holds the
/// values of the field in a document-id order.
///
/// Port of `FieldExistsWeight` (`core/search/field_exists_query.rs`).
///
/// **Deviation:** the reference implementation's vector branch of `count` is not
/// ported (the port has no vector values), so such a field falls back to the
/// scorer, which reports that vectors are unsupported.
export class FieldExistsWeight final : public Weight {
public:
    /// The weight of `query`, with the given score and score mode.
    FieldExistsWeight(const FieldExistsQuery &query, float score, ScoreMode score_mode);

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                              const IndexSearcher &searcher) const override;

    [[nodiscard]] const Query &get_query() const override { return query_; }

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext &context,
                                                                         const IndexSearcher &searcher) override;

    /// Counts the matching documents without visiting them when the leaf can
    /// answer it directly: dense norms, the point count or the term count.
    [[nodiscard]] Result<int32_t> count(const LeafReaderContext &context, const IndexSearcher &searcher) override;

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext & /*context*/) const override { return true; }

private:
    [[nodiscard]] Result<std::unique_ptr<Scorer>> scorer_for(const LeafReaderContext &context) const;

    const FieldExistsQuery &query_;
    float score_{1.0F};
    ScoreMode score_mode_{ScoreMode::Complete};
};

} // namespace lucene
