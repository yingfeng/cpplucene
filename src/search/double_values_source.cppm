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

export module lucene_core:double_values_source;

import :types;
import std;

import :double_values;
import :error;
import :explanation;
import :leaf_reader_context;
import :sort_field;

namespace lucene {

export class IndexSearcher;

/// A producer of per-document `double` values, independent of any one reader.
///
/// Port of the `DoubleValuesSource` trait (`search/double_values_source.rs`).
///
/// **Deviations:** the associated types collapse into `std::unique_ptr`
/// (values, rewritten sources, sort fields) and the reader context is the
/// concrete [`LeafReaderContext`] of this port.
export class DoubleValuesSource {
public:
    virtual ~DoubleValuesSource() = default;

    /// Values for `ctx`.  `scores` may be null when [`needs_scores`] is false.
    [[nodiscard]] virtual Result<std::unique_ptr<DoubleValues>>
    get_values(const LeafReaderContext &ctx, std::unique_ptr<DoubleValues> scores) const = 0;

    /// Whether [`get_values`] needs the scores of the current document.
    [[nodiscard]] virtual bool needs_scores() const noexcept = 0;

    /// Rewrites this source for `searcher`; returns itself when the source does
    /// not depend on the reader.
    [[nodiscard]] virtual Result<std::unique_ptr<DoubleValuesSource>> rewrite(const IndexSearcher &searcher) const;

    /// Explains the value of `doc_id`; the default reports that explanations are
    /// not implemented.
    [[nodiscard]] virtual Result<Explanation> explain(const LeafReaderContext &ctx, int32_t doc_id,
                                                      const Explanation &score_explanation) const;

    /// A sort field over this source, or `nullptr` when it cannot be sorted on.
    [[nodiscard]] virtual Result<std::unique_ptr<SortField>> get_sort_field(bool reverse) const;

    /// Whether the source is cacheable for `ctx` (part of `SegmentCacheable`).
    [[nodiscard]] virtual bool is_cacheable(const LeafReaderContext &ctx) const { return true; }
};

} // namespace lucene
