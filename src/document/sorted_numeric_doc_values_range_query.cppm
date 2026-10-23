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

export module lucene_core:sorted_numeric_doc_values_range_query;

import :types;
import std;

import :error;
import :query;
import :query_visitor;
import :score_mode;
import :weight;

namespace lucene {

export class IndexSearcher;

/// Matches the documents whose sorted numeric doc values contain a value in
/// `[lower_value, upper_value]`.
///
/// Port of `SortedNumericDocValuesRangeQuery`
/// (`core/document/sorted_numeric_doc_values_range_query.rs`).
///
/// Like the set query it is a constant-score query over a two-phase iterator,
/// but it also uses the doc values skipper to skip whole intervals whose values
/// are outside the range.
///
/// **Deviations** (C++ mechanics only):
///  * the reference implementation's `get_doc_id_set_iterator_or_null_for_primary_sort`
///    fast path (a sorted index reused as the iterator) is not ported: the port
///    has no numeric sorter yet;
///  * `Identity` and `Accountable` are not ported, as everywhere else.
export class SortedNumericDocValuesRangeQuery final : public Query {
public:
    /// `lower_value` must not be greater than `upper_value`; the full range is
    /// rewritten to a `FieldExistsQuery`.
    [[nodiscard]] static Result<SortedNumericDocValuesRangeQuery> create(std::string field, int64_t lower_value,
                                                                        int64_t upper_value);

    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    [[nodiscard]] int64_t lower_value() const noexcept { return lower_value_; }
    [[nodiscard]] int64_t upper_value() const noexcept { return upper_value_; }

    // ---- Query ----

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const override;

    [[nodiscard]] Result<std::shared_ptr<Query>> rewrite(const IndexSearcher &searcher) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    SortedNumericDocValuesRangeQuery(std::string field, int64_t lower_value, int64_t upper_value);

    std::string field_;
    int64_t lower_value_;
    int64_t upper_value_;
};

} // namespace lucene
