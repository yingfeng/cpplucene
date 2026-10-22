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

export module lucene_core:query;

import :types;
import std;

import :error;
import :identity;
import :query_visitor;
import :score_mode;
import :weight;

namespace lucene {

export class IndexSearcher;

/// The base of every query.
///
/// Port of `QueryBase` + the `Query` enum (`core/search/query.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * rlucene models the query tree as one `Query` enum with a variant per
///    concrete query and dispatches with `match`; here `Query` is an abstract
///    base and every concrete query is a subclass, which is the same
///    type erasure the rest of the port uses;
///  * `create_weight` takes `const Query&` (rlucene consumes `self` into an
///    `Arc`, which the `Weight` then owns): the caller must keep the query alive
///    while its weight is in use;
///  * `rewrite` returns a `shared_ptr` (null when the query is unchanged);
///  * `PartialEq`/`Hash` of the Rust enum become [`equals`] / [`hash`], which a
///    query cache keys on.
export class Query {
public:
    virtual ~Query() = default;

    /// Builds the searcher-dependent state of this query.
    [[nodiscard]] virtual Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher,
                                                                        ScoreMode score_mode, float boost) const = 0;

    /// Returns a rewritten query, or a null pointer when this query is already
    /// in its final form.  The default leaves the query unchanged.
    [[nodiscard]] virtual Result<std::shared_ptr<Query>> rewrite(const IndexSearcher &searcher) const {
        (void)searcher;
        return std::shared_ptr<Query>{};
    }

    /// Recurses through the query tree with `visitor`.
    [[nodiscard]] virtual VoidResult visit(QueryVisitor &visitor) const = 0;

    /// The `field:term` form used in `explain` and `toString`; `field` is the
    /// field the caller is interested in, and is omitted from the text when it
    /// matches.
    [[nodiscard]] virtual Result<std::string> to_string(std::string_view field) const = 0;

    /// This query instance's identity; see [`Identity`].
    [[nodiscard]] const Identity &identity() const noexcept { return identity_; }

    /// Value equality, the counterpart of rlucene's `PartialEq` on the enum.
    [[nodiscard]] virtual bool equals(const Query &other) const = 0;

    /// Hash consistent with [`equals`].
    [[nodiscard]] virtual size_t hash() const = 0;

protected:
    Query() = default;

private:
    Identity identity_{};
};

} // namespace lucene
