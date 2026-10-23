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

export module lucene_core:prefix_query;

import :types;
import std;

import :error;
import :multi_term_query;
import :terms;
import :terms_enum;

namespace lucene {

/// Matches the documents whose field has a term starting with a prefix.
///
/// Port of `PrefixQuery` (`core/search/prefix_query.rs`).
///
/// **Deviation:** the expansion walks the terms of a leaf with a predicate
/// instead of the reference implementation's `PrefixTermsEnum`/`FilteredTermsEnum` pair, because
/// the block tree has no prefix seek specialization in this port yet.
export class PrefixQuery final : public MultiTermQuery {
public:
    /// A query for the terms of `field` starting with `prefix`.
    PrefixQuery(std::string field, std::string prefix,
                MultiTermRewriteMethod rewrite_method = MultiTermRewriteMethod::kConstantScore);

    /// The prefix, as the bytes the terms are compared against.
    [[nodiscard]] const std::string &prefix() const noexcept { return pattern(); }

protected:
    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> terms_enum(Terms &terms) const override;

    [[nodiscard]] std::string_view query_name() const override { return "PrefixQuery"; }
};

} // namespace lucene
