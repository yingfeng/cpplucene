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

export module lucene_core:query_visitor;

import :types;
import std;

import :error;
import :term;

namespace lucene {

export class Query;

/// Recursion hook of [`Query::visit`].
///
/// Port of the `QueryVisitor` trait (`core/search/query_visitor.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * `consume_terms` takes a `std::span<const Term>` instead of a generic
///    `&[T] where T: Borrow<Term>`; every call site in the reference implementation passes terms;
///  * `consume_terms_matching` is not ported yet — it needs an automaton
///    supplier, and no ported query uses it (the automaton queries land later);
///  * `get_sub_visitor` / `Occur` are not ported yet: they exist for
///    `BooleanQuery`, which is not ported.  Until then a composite query that
///    wants to prune `MUST_NOT` children has to do it itself.
export class QueryVisitor {
public:
    virtual ~QueryVisitor() = default;

    /// Called by leaf queries that match on specific terms.
    [[nodiscard]] virtual VoidResult consume_terms(const Query &query, std::span<const Term> terms) {
        return {};
    }

    /// Called by leaf queries that do not match on terms.
    [[nodiscard]] virtual VoidResult visit_leaf(const Query &query) { return {}; }

    /// Whether this field is of interest to the visitor; lets a visitor avoid
    /// collecting terms from fields it does not care about.
    [[nodiscard]] virtual bool accept_field(std::string_view field) const {
        (void)field;
        return true;
    }
};

} // namespace lucene
