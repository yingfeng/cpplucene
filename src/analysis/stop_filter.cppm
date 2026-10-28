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

export module lucene_core:stop_filter;

import :types;
import :error;

import std;

import :attributes;
import :char_array_set;
import :token_stream;

namespace lucene {

/// Removes tokens that appear in a stop word set.
///
/// Port of `analysis/stop_filter.rs`.  Removed tokens still advance the position
/// of the following token, exactly like Lucene: the skipped position increments
/// are accumulated and added to the next emitted token.
export class StopFilter : public TokenFilter {
public:
    StopFilter(std::unique_ptr<TokenStream> input, std::set<std::string, std::less<>> stop_words);

    /// Takes the stop words as a [`CharArraySet`], which is what the reference
    /// implementation's `StopFilter` takes: a set built once can be shared by every
    /// analyzer that wants it, and a case-insensitive set stays case-insensitive.
    StopFilter(std::unique_ptr<TokenStream> input, const CharArraySet &stop_words);

    [[nodiscard]] Result<bool> increment_token() override;

    [[nodiscard]] const std::set<std::string, std::less<>> &stop_words() const noexcept { return stop_words_; }

    /// The set this filter was built from, when it was given one.
    [[nodiscard]] const CharArraySet *char_array_stop_words() const noexcept {
        return char_stop_words_.has_value() ? &char_stop_words_.value() : nullptr;
    }

    /// The default English stop word list (`StandardAnalyzer.STOP_WORDS_SET`).
    [[nodiscard]] static const std::set<std::string, std::less<>> &english_stop_words();

private:
    /// Whether `word` is a stop word.
    [[nodiscard]] bool is_stop_word(std::string_view word) const;

    std::set<std::string, std::less<>> stop_words_;
    /// The set a caller handed in, kept as it is so its case folding applies.
    std::optional<CharArraySet> char_stop_words_;
    uint32_t skipped_positions_{0};
};

} // namespace lucene
