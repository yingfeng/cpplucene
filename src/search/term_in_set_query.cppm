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

export module lucene_core:term_in_set_query;

import :types;
import std;

import :error;
import :multi_term_query;
import :prefix_coded_terms;
import :query;
import :query_visitor;
import :terms;
import :terms_enum;

namespace lucene {

/// Matches the documents that contain one of a fixed set of terms.
///
/// Port of `TermInSetQuery` (`core/search/term_in_set_query.rs`).  The terms are
/// stored in a [`PrefixCodedTerms`], so a query with thousands of terms stays
/// compact; the terms have to be added in increasing order.
///
/// The query is a [`MultiTermQuery`], so the rewrite methods of
/// [`MultiTermRewriteMethod`] apply: by default the constant score rewrite is
/// used, which keeps the expansion from overflowing the boolean clause limit.
export class TermInSetQuery final : public MultiTermQuery {
public:
    /// Creates the query; it has to be owned by a `shared_ptr`, because the
    /// constant score rewrite wraps the query itself.
    [[nodiscard]] static Result<std::shared_ptr<TermInSetQuery>>
    create(std::string field, std::shared_ptr<const PrefixCodedTerms> term_data,
           MultiTermRewriteMethod rewrite_method = MultiTermRewriteMethod::kConstantScoreBlended);

    /// The terms of the query.
    [[nodiscard]] const std::shared_ptr<const PrefixCodedTerms> &term_data() const noexcept { return term_data_; }

    [[nodiscard]] int64_t terms_count() const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

protected:
    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> terms_enum(Terms &terms) const override;

    [[nodiscard]] std::string_view query_name() const override { return "TermInSetQuery"; }

    [[nodiscard]] bool peers(const MultiTermQuery &other) const override;

private:
    TermInSetQuery(std::string field, std::shared_ptr<const PrefixCodedTerms> term_data,
                   MultiTermRewriteMethod rewrite_method);

    std::shared_ptr<const PrefixCodedTerms> term_data_;
    size_t term_data_hash_{0};
};

} // namespace lucene
