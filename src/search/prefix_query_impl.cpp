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

module lucene_core:prefix_query.impl;

import :prefix_query;

import std;

import :error;
import :multi_term_query;
import :terms;
import :terms_enum;
import :types;

namespace lucene {

namespace {

/// `true` when `term` starts with `prefix`; terms are sorted, so the first term
/// greater than the prefix that does not start with it ends the enumeration.
[[nodiscard]] bool starts_with(const BytesRef &term, const BytesRef &prefix) {
    const std::span<const uint8_t> bytes = term.as_slice();
    return bytes.size() >= prefix.length() && std::ranges::equal(prefix.as_slice(), bytes.subspan(0, prefix.length()));
}

[[nodiscard]] bool past_prefix(const BytesRef &term, const BytesRef &prefix) {
    return term.compare_to(prefix) > 0 && !starts_with(term, prefix);
}

} // namespace

PrefixQuery::PrefixQuery(std::string field, std::string prefix, MultiTermRewriteMethod rewrite_method)
        : MultiTermQuery(std::move(field), std::move(prefix), rewrite_method) {}

Result<std::unique_ptr<TermsEnum>> PrefixQuery::terms_enum(Terms &terms) const {
    auto inner = terms.iterator();
    if (!inner.has_value()) {
        return std::unexpected(std::move(inner.error()));
    }
    const BytesRef prefix = BytesRef::from_string(pattern());
    std::unique_ptr<TermsEnum> filtered(new PredicateTermsEnum(
            std::move(inner.value()), [prefix](const BytesRef &term) { return starts_with(term, prefix); },
            [prefix](const BytesRef &term) { return past_prefix(term, prefix); }));
    return filtered;
}

} // namespace lucene
