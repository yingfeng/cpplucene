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

export module lucene_core:term_range_query;

import :types;
import std;

import :automaton;
import :automaton_query;
import :bytes_ref;
import :compiled_automaton;
import :error;
import :multi_term_query;
import :postings_enum;
import :terms;
import :terms_enum;

namespace lucene {

/// A query over the terms of a field that fall inside a range, compared as
/// [`BytesRef`].
///
/// Port of `TermRangeQuery` (`core/search/term_range_query.rs`).
///
/// This query is much slower than a point based range, because it visits every
/// term of the range and merges their matches.
///
/// An open endpoint (`nullopt`) cannot be exclusive: it says nothing about which
/// term to leave out, so the corresponding `include` flag is forced to `true`.
///
/// **Deviation:** the reference implementation's constructors become static
/// factories returning `Result`, because compiling the range automaton can fail.
export class TermRangeQuery final : public AutomatonQuery {
public:
    /// A query over the terms of `field` in the range, with the default rewrite
    /// method.
    [[nodiscard]] static Result<TermRangeQuery> create(std::string field, std::optional<BytesRef> lower_term,
                                                      std::optional<BytesRef> upper_term, bool include_lower,
                                                      bool include_upper);

    /// [`create`], with the rewrite method to expand the matching terms with.
    [[nodiscard]] static Result<TermRangeQuery> with_rewrite(std::string field, std::optional<BytesRef> lower_term,
                                                            std::optional<BytesRef> upper_term, bool include_lower,
                                                            bool include_upper,
                                                            MultiTermRewriteMethod rewrite_method);

    /// [`create`] over term texts.
    [[nodiscard]] static Result<TermRangeQuery> create_string_range(std::string field,
                                                                   std::optional<std::string> lower_term,
                                                                   std::optional<std::string> upper_term,
                                                                   bool include_lower, bool include_upper);

    /// [`create_string_range`], with the rewrite method.
    [[nodiscard]] static Result<TermRangeQuery> with_string_range_rewrite(std::string field,
                                                                         std::optional<std::string> lower_term,
                                                                         std::optional<std::string> upper_term,
                                                                         bool include_lower, bool include_upper,
                                                                         MultiTermRewriteMethod rewrite_method);

    /// The automaton a range accepts: the binary interval between the bounds.
    [[nodiscard]] static Result<Automaton> to_automaton(const BytesRef *lower_term, const BytesRef *upper_term,
                                                        bool include_lower, bool include_upper);

    /// The lower endpoint, or `nullopt` when the range is open below.
    [[nodiscard]] const std::optional<BytesRef> &lower_term() const noexcept { return lower_term_; }
    /// The upper endpoint, or `nullopt` when the range is open above.
    [[nodiscard]] const std::optional<BytesRef> &upper_term() const noexcept { return upper_term_; }
    /// Whether the lower endpoint is part of the range.
    [[nodiscard]] bool includes_lower() const noexcept { return include_lower_; }
    /// Whether the upper endpoint is part of the range.
    [[nodiscard]] bool includes_upper() const noexcept { return include_upper_; }

    // ---- Query ----

    /// `field:[lower TO upper]`, with `{`/`}` for an exclusive endpoint and `*`
    /// for an open one, the form the reference implementation prints.
    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

protected:
    [[nodiscard]] std::string_view query_name() const override { return "TermRangeQuery"; }

    [[nodiscard]] bool peers(const MultiTermQuery &other) const override;

private:
    TermRangeQuery(std::optional<BytesRef> lower_term, std::optional<BytesRef> upper_term, bool include_lower,
                   bool include_upper, Term term, CompiledAutomaton compiled, MultiTermRewriteMethod rewrite_method);

    std::optional<BytesRef> lower_term_;
    std::optional<BytesRef> upper_term_;
    bool include_lower_{true};
    bool include_upper_{true};
};

} // namespace lucene
