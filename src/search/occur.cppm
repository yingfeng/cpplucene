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

// `Occur` lives in its own module so that `:query_visitor` can ask for a child
// visitor without importing `:boolean_clause` (which imports `:query`, and
// `:query` imports `:query_visitor`).  `:boolean_clause` re-exports it.

export module lucene_core:occur;

import :types;
import std;

namespace lucene {

/// How often a clause has to occur in a matching document.
///
/// Port of `Occur` (`core/search/boolean_clause.rs`).
///
/// The numeric values are the slot indices `BooleanQuery` uses for its four
/// clause lists, so they must not be reordered.
export enum class Occur : int32_t {
    /// The clause must appear, and it contributes to the score.
    Must = 0,
    /// The clause must appear but does not contribute to the score.
    Filter = 1,
    /// The clause may appear; when the query has no `MUST`/`FILTER` clause at
    /// least `minimum_should_match` of them must match.
    Should = 2,
    /// The clause must not appear; it does not contribute to the score.
    MustNot = 3,
};

/// Whether the clause has to match.
[[nodiscard]] constexpr bool occur_is_required(Occur occur) noexcept {
    return occur == Occur::Must || occur == Occur::Filter;
}

/// Whether the clause contributes to the score.
[[nodiscard]] constexpr bool occur_is_scoring(Occur occur) noexcept {
    return occur == Occur::Must || occur == Occur::Should;
}

/// Whether the clause excludes documents.
[[nodiscard]] constexpr bool occur_is_prohibited(Occur occur) noexcept { return occur == Occur::MustNot; }

} // namespace lucene
