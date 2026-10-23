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

export module lucene_core:sorted_set_doc_values_range_query;

import :types;
import std;

import :bytes_ref;
import :error;
import :query;
import :query_visitor;
import :score_mode;
import :weight;

namespace lucene {

export class IndexSearcher;

/// Matches the documents whose sorted set doc values contain an ordinal in
/// `[min_ord, max_ord]`, where the bounds are the ordinals of the query terms.
///
/// Port of `SortedSetDocValuesRangeQuery`
/// (`core/document/sorted_set_doc_values_range_query.rs`).
///
/// An open endpoint is unbounded; a closed endpoint that is not in the
/// dictionary narrows the ordinal range to the insertion point, which is why a
/// missing term still matches the neighbouring values.
///
/// **Deviations** (C++ mechanics only):
///  * the reference implementation's `get_doc_id_set_iterator_or_null_for_primary_sort`
///    fast path (the sorted index reused as the iterator) is not ported: the port
///    has no numeric sorter yet, so the single-valued case uses the generic
///    sorted-set two-phase iterator, which is functionally equivalent;
///  * `Identity` and `Accountable` are not ported, as everywhere else.
export class SortedSetDocValuesRangeQuery final : public Query {
public:
    /// `lower_inclusive`/`upper_inclusive` are ignored for open endpoints, as in
    /// the reference implementation.
    [[nodiscard]] static Result<SortedSetDocValuesRangeQuery>
    create(std::string field, std::optional<BytesRef> lower_value, std::optional<BytesRef> upper_value,
           bool lower_inclusive, bool upper_inclusive);

    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    [[nodiscard]] const std::optional<BytesRef> &lower_value() const noexcept { return lower_value_; }
    [[nodiscard]] const std::optional<BytesRef> &upper_value() const noexcept { return upper_value_; }
    [[nodiscard]] bool lower_inclusive() const noexcept { return lower_inclusive_; }
    [[nodiscard]] bool upper_inclusive() const noexcept { return upper_inclusive_; }

    // ---- Query ----

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                               float boost) const override;

    /// The fully open range is rewritten to a `FieldExistsQuery`.
    [[nodiscard]] Result<std::shared_ptr<Query>> rewrite(const IndexSearcher &searcher) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    SortedSetDocValuesRangeQuery(std::string field, std::optional<BytesRef> lower_value,
                                 std::optional<BytesRef> upper_value, bool lower_inclusive, bool upper_inclusive);

    std::string field_;
    std::optional<BytesRef> lower_value_;
    std::optional<BytesRef> upper_value_;
    bool lower_inclusive_;
    bool upper_inclusive_;
};

} // namespace lucene
