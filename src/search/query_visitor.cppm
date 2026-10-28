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

import :byte_run_automaton;
import :error;
import :occur;
import :term;

namespace lucene {

export class Query;

/// Supplies the automaton of the terms a visitor is told about, or `nullopt` when
/// the terms are unbounded.  A supplier keeps a query from building an automaton
/// the visitor never looks at.
export using AutomatonSupplier = std::function<Result<std::optional<ByteRunAutomaton>>()>;

/// Recursion hook of [`Query::visit`].
///
/// Port of the `QueryVisitor` trait (`core/search/query_visitor.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * `consume_terms` takes a `std::span<const Term>` instead of a generic
///    `&[T] where T: Borrow<Term>`; every call site in the reference implementation passes terms;
///  * `get_sub_visitor` answers a [`DefaultQueryVisitor`] by value instead of a
///    lifetime-bound associated type: C++ has no `GAT`, so the visitor is borrowed
///    through a wrapper the caller keeps on the stack.
export class DefaultQueryVisitor;

export class QueryVisitor {
public:
    virtual ~QueryVisitor() = default;

    /// Called by leaf queries that match on specific terms.
    [[nodiscard]] virtual VoidResult consume_terms(const Query &query, std::span<const Term> terms) {
        return {};
    }

    /// Called by leaf queries that match on a class of terms.
    ///
    /// The default implementation falls back to [`visit_leaf`], which is what the
    /// reference implementation does for backwards compatibility.
    [[nodiscard]] virtual VoidResult consume_terms_matching(const Query &query, std::string_view field,
                                                            const AutomatonSupplier &automaton) {
        (void)field;
        (void)automaton;
        return visit_leaf(query);
    }

    /// Called by leaf queries that do not match on terms.
    [[nodiscard]] virtual VoidResult visit_leaf(const Query &query) { return {}; }

    /// Whether this field is of interest to the visitor; lets a visitor avoid
    /// collecting terms from fields it does not care about.
    [[nodiscard]] virtual bool accept_field(std::string_view field) const {
        (void)field;
        return true;
    }

    /// Pulls the visitor of the child clauses of `parent`: this visitor, or one
    /// that ignores everything when `occur` is [`Occur::MustNot`].
    ///
    /// Port of `QueryVisitor::get_sub_visitor` (`core/search/query_visitor.rs`).
    [[nodiscard]] virtual DefaultQueryVisitor get_sub_visitor(Occur occur, const Query &parent);
};

/// The visitor [`QueryVisitor::get_sub_visitor`] answers: it forwards to the
/// parent visitor, or ignores every call when it wraps none (a `MUST_NOT`
/// clause).
///
/// Port of `DefaultQueryVisitor` (`core/search/query_visitor.rs`).
export class DefaultQueryVisitor final : public QueryVisitor {
public:
    /// Wraps `visitor`; a null visitor ignores everything.
    explicit DefaultQueryVisitor(QueryVisitor *visitor) noexcept : visitor_(visitor) {}

    [[nodiscard]] VoidResult consume_terms(const Query &query, std::span<const Term> terms) override;

    [[nodiscard]] VoidResult consume_terms_matching(const Query &query, std::string_view field,
                                                    const AutomatonSupplier &automaton) override;

    [[nodiscard]] VoidResult visit_leaf(const Query &query) override;

    [[nodiscard]] bool accept_field(std::string_view field) const override;

    [[nodiscard]] DefaultQueryVisitor get_sub_visitor(Occur occur, const Query &parent) override;

    /// Whether the visitor wraps a parent; `false` for a `MUST_NOT` clause.
    [[nodiscard]] bool is_empty() const noexcept { return visitor_ == nullptr; }

private:
    QueryVisitor *visitor_{nullptr};
};

} // namespace lucene
