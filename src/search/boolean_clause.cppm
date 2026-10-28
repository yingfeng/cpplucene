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

export module lucene_core:boolean_clause;

import :types;

import std;

import :query;
import :occur;

export import :occur;

namespace lucene {

/// The one-character prefix [`BooleanQuery::to_string`] uses: `+`, `#`, `""`, `-`.
[[nodiscard]] std::string_view occur_prefix(Occur occur);

/// One clause of a [`BooleanQuery`](lucene_core:boolean_query).
///
/// Port of `BooleanClause` (`core/search/boolean_clause.rs`).
///
/// **Deviation:** the query is a `shared_ptr`, because the port's `Weight`
/// borrows its query (the reference implementation owns it through an `Arc` inside the `Query` enum);
/// sharing keeps the clause copyable and the query alive while a weight uses it.
export struct BooleanClause {
    std::shared_ptr<Query> query;
    Occur occur{Occur::Should};

    [[nodiscard]] static BooleanClause create(std::shared_ptr<Query> query, Occur occur) {
        return BooleanClause{std::move(query), occur};
    }

    [[nodiscard]] bool is_required() const noexcept { return occur_is_required(occur); }
    [[nodiscard]] bool is_scoring() const noexcept { return occur_is_scoring(occur); }
    [[nodiscard]] bool is_prohibited() const noexcept { return occur_is_prohibited(occur); }
};

} // namespace lucene
