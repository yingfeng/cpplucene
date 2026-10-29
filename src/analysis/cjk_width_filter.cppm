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

export module lucene_core:cjk_width_filter;

import :token_stream;
import :types;

import std;

namespace lucene {

/// Folds the width of a term into its canonical form: full width ASCII and the
/// ideographic space become their ASCII counterparts, and half width katakana
/// becomes full width katakana.
///
/// Port of `CJKWidthFilter` (`analysis/common/cjk/cjk_width_filter.rs`).  Like
/// the reference implementation it is a no-op when the term holds no wide
/// character, which is the common case for Latin text.
export class CJKWidthFilter : public TokenFilter {
public:
    explicit CJKWidthFilter(std::unique_ptr<TokenStream> input);

    [[nodiscard]] Result<bool> increment_token() override;

    /// Folds `text` (exposed for tests and for callers that need the mapping
    /// without a token stream).
    [[nodiscard]] static std::string fold(std::string_view text);
};

} // namespace lucene
