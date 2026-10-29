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

export module lucene_core:ngram_tokenizer;

import :tokenizer;
import :types;

import std;

namespace lucene {

/// Splits the whole input into every n-gram of length `[min_gram, max_gram]`,
/// in the order the reference implementation emits them: by start offset first,
/// then by increasing length (`abc` gives `a`, `ab`, `abc`, `b`, `bc`, `c`).
///
/// Port of `NGramTokenizer` (`analysis/common/ngram/ngram_tokenizer.rs`).
export class NGramTokenizer : public Tokenizer {
public:
    /// `min_gram` must be at least 1 and must not exceed `max_gram`.
    NGramTokenizer(int32_t min_gram, int32_t max_gram);

    [[nodiscard]] VoidResult reset() override;
    [[nodiscard]] Result<bool> increment_token() override;

    [[nodiscard]] int32_t min_gram() const noexcept { return min_gram_; }
    [[nodiscard]] int32_t max_gram() const noexcept { return max_gram_; }

private:
    int32_t min_gram_;
    int32_t max_gram_;
    size_t gram_start_{0};
    int32_t gram_size_{0};
    bool started_{false};
};

/// Like [`NGramTokenizer`], but only the n-grams that start at the first
/// character: `abc` gives `a`, `ab`, `abc`.
///
/// Port of `EdgeNGramTokenizer`.
export class EdgeNGramTokenizer : public Tokenizer {
public:
    EdgeNGramTokenizer(int32_t min_gram, int32_t max_gram);

    [[nodiscard]] VoidResult reset() override;
    [[nodiscard]] Result<bool> increment_token() override;

private:
    int32_t min_gram_;
    int32_t max_gram_;
    int32_t gram_size_{0};
    bool started_{false};
};

} // namespace lucene
