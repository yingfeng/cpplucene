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

export module lucene_core:shingle_filter;

import :token_stream;
import :types;

import std;

namespace lucene {

/// Emits, for every input position, the runs of `[min_shingle_size,
/// max_shingle_size]` consecutive tokens that start there, joined with
/// [`token_separator`].
///
/// Port of `ShingleFilter` (`analysis/common/shingle/shingle_filter.rs`).  With
/// the defaults (`min = max = 2`, unigrams on) `a b c` becomes
/// `a`, `a b`, `b`, `b c`, `c`: every shingle shares the position of its first
/// token.
///
/// **Deviations** from the reference implementation:
///
/// 1. The whole field value is read before anything is emitted, so the filter is
///    not streaming.  A field value is held in memory by the tokenizer anyway,
///    so this only costs one extra copy of the terms.
/// 2. No filler tokens: a position increment greater than one starts a new
///    shingle group instead of being padded with `fillerToken`, and a trailing
///    partial window does not produce a shorter shingle.
/// 3. `outputUnigramsIfNoShingles` and `PositionLengthAttribute` are not set.
export class ShingleFilter : public TokenFilter {
public:
    /// `min_shingle_size` must be at least 2 and must not exceed
    /// `max_shingle_size`; an invalid range is clamped.
    ShingleFilter(std::unique_ptr<TokenStream> input, int32_t min_shingle_size = 2, int32_t max_shingle_size = 2,
                  bool output_unigrams = true);

    /// The string put between the tokens of a shingle; the default is a space.
    void set_token_separator(std::string_view separator) { separator_ = std::string(separator); }
    [[nodiscard]] const std::string &token_separator() const noexcept { return separator_; }

    [[nodiscard]] VoidResult reset() override;
    [[nodiscard]] Result<bool> increment_token() override;

private:
    struct PendingToken {
        std::string term;
        uint32_t position_increment{0};
    };

    [[nodiscard]] VoidResult build_output();

    int32_t min_shingle_size_;
    int32_t max_shingle_size_;
    bool output_unigrams_;
    std::string separator_{" "};
    std::deque<PendingToken> output_;
    bool loaded_{false};
};

} // namespace lucene
