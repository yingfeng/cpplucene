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

export module lucene_core:single_terms_enum;

import :types;
import std;

import :bytes_ref;
import :error;
import :filtered_terms_enum;
import :terms_enum;

namespace lucene {

/// A [`FilteredTermsEnum`] hook that enumerates a single term.
///
/// For example this is used by `MultiTermQuery` implementations that only need
/// to visit one term but want to keep the `MultiTermQuery` semantics.
///
/// Port of `SingleTermsEnum` (`core/index/single_terms_enum.rs`).
export class SingleTermsEnum final : public FilteredTermsEnumBase {
public:
    /// Wraps `tenum` so that only `term_text` is ever returned.
    [[nodiscard]] static std::unique_ptr<FilteredTermsEnum> create(std::unique_ptr<TermsEnum> tenum, BytesRef term_text);

    explicit SingleTermsEnum(BytesRef single);

    [[nodiscard]] Result<AcceptStatus> accept(const BytesRef &term, int64_t ord) override;
    [[nodiscard]] Result<std::optional<BytesRef>> next_seek_term(const BytesRef *current) override;

private:
    BytesRef single_;
};

} // namespace lucene
