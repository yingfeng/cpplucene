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

export module lucene_core:sorted_numeric_doc_values_set_query;

import :types;
import std;

import :doc_values_long_hash_set;
import :error;
import :query;
import :query_visitor;
import :score_mode;
import :weight;

namespace lucene {

export class IndexSearcher;

/// Matches the documents whose sorted numeric doc values contain one of a set of
/// numbers.
///
/// Port of `SortedNumericDocValuesSetQuery`
/// (`core/document/sorted_numeric_doc_values_set_query.rs`).
///
/// The query is a constant-score query over a two-phase iterator: the doc values
/// give the candidates, and the set decides which candidates match.
///
/// **Deviation:** the reference implementation keeps an `Identity` and can
/// `explain` through its `ConstantScoreWeight`; here the identity is implicit
/// (the query object) and `explain` reads the doc values directly.
export class SortedNumericDocValuesSetQuery final : public Query {
public:
    /// Builds the query; `numbers` may be unsorted and may repeat.
    [[nodiscard]] static Result<SortedNumericDocValuesSetQuery> create(std::string field,
                                                                      std::vector<int64_t> numbers);

    /// The field whose doc values are checked.
    [[nodiscard]] const std::string &field() const noexcept { return field_; }

    /// The numbers to match, as a hash set.
    [[nodiscard]] const DocValuesLongHashSet &numbers() const noexcept { return *numbers_; }

    /// The numbers, shared with the weight and its iterators.
    [[nodiscard]] const std::shared_ptr<const DocValuesLongHashSet> &numbers_ptr() const noexcept { return numbers_; }

    // ---- Query ----

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const override;

    [[nodiscard]] Result<std::shared_ptr<Query>> rewrite(const IndexSearcher &searcher) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    SortedNumericDocValuesSetQuery(std::string field, std::shared_ptr<const DocValuesLongHashSet> numbers);

    std::string field_;
    std::shared_ptr<const DocValuesLongHashSet> numbers_;
};

} // namespace lucene
