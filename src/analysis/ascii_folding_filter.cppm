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

export module lucene_core:ascii_folding_filter;

import :token_stream;
import :types;

import std;

namespace lucene {

/// Folds accented Latin letters into their ASCII base letters, so that `café`
/// and `cafe` index as the same term.
///
/// Port of `ASCIIFoldingFilter` (`analysis/common/miscellaneous/ascii_folding_filter.rs`).
///
/// **Deviation:** the reference implementation folds the whole Unicode repertoire
/// (Latin Extended Additional, Greek, Cyrillic, punctuation, …); this port covers
/// **Latin-1 Supplement** and **Latin Extended-A**, which is what the Western
/// European languages need.  Code points outside those blocks are left alone.
export class ASCIIFoldingFilter : public TokenFilter {
public:
    /// `preserve_original` emits the folded term and then the original one at the
    /// same position, like the reference implementation does.
    explicit ASCIIFoldingFilter(std::unique_ptr<TokenStream> input, bool preserve_original = false);

    [[nodiscard]] VoidResult reset() override;
    [[nodiscard]] Result<bool> increment_token() override;

    /// Folds the characters of `text` (exposed for tests and for callers that
    /// need the mapping without a token stream).
    [[nodiscard]] static std::string fold(std::string_view text);

private:
    bool preserve_original_;
    std::string pending_;
    bool has_pending_{false};
};

} // namespace lucene
