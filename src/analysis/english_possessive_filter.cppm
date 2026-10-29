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

export module lucene_core:english_possessive_filter;

import :token_stream;
import :types;

import std;

namespace lucene {

/// Strips the English possessive ending from a term: `John's` becomes `John`,
/// and so do `Johns'` and the typographic apostrophe variants.
///
/// Port of `EnglishPossessiveFilter` (`analysis/common/en/english_possessive_filter.rs`).
export class EnglishPossessiveFilter : public TokenFilter {
public:
    explicit EnglishPossessiveFilter(std::unique_ptr<TokenStream> input);

    [[nodiscard]] Result<bool> increment_token() override;
};

} // namespace lucene
