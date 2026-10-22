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

export module lucene_core:lower_case_filter;

import :types;
import :error;

import std;

import :attributes;
import :token_stream;

namespace lucene {

/// Normalizes every token to lower case.  Port of `analysis/lower_case_filter.rs`.
///
/// Uses full Unicode simple case folding for the scripts covered by
/// [`unicode_util`]; ASCII and Latin-1 are exact.
export class LowerCaseFilter : public TokenFilter {
public:
    explicit LowerCaseFilter(std::unique_ptr<TokenStream> input);

    [[nodiscard]] Result<bool> increment_token() override;
};

/// Lower-cases a single code point (simple case mapping).
[[nodiscard]] char32_t to_lower_case(char32_t codepoint) noexcept;

} // namespace lucene
