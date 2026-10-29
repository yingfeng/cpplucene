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

export module lucene_core:cjk_bigram_filter;

import :token_stream;
import :types;

import std;

namespace lucene {

/// Turns runs of CJK characters into overlapping bigrams, which is how Lucene
/// indexes languages that are written without spaces.
///
/// Port of `CJKBigramFilter` (`analysis/common/cjk/cjk_bigram_filter.rs`).  A run
/// of `n` characters of one script (Han, Hiragana, Katakana or Hangul) becomes
/// `n - 1` bigrams, so `中国人` is indexed as `中国` and `国人`; a run of one
/// character stays a unigram.  Runs of different scripts are not mixed, and a
/// term that is not CJK at all passes through untouched.
///
/// **Deviations** from the reference implementation:
///
/// 1. The script of a term is decided from its code points instead of the
///    `TypeAttribute` the tokenizer attaches (`<HAN>`, `<HIRAGANA>`, …), so this
///    filter also works behind a tokenizer that does not label its types.
/// 2. The `outputUnigrams` variant is not ported: bigrams only.
/// 3. Offsets and payloads are not split along with the term; a bigram keeps the
///    position increment of the first character and no offsets.
export class CJKBigramFilter : public TokenFilter {
public:
    explicit CJKBigramFilter(std::unique_ptr<TokenStream> input);

    [[nodiscard]] VoidResult reset() override;
    [[nodiscard]] Result<bool> increment_token() override;

private:
    struct PendingToken {
        std::string term;
        uint32_t position_increment{1};
    };

    void flush_run();
    void fold_into_run(PendingToken token);

    std::deque<PendingToken> output_;
    std::vector<PendingToken> run_;
    int8_t script_{-1};
    bool exhausted_{false};
};

} // namespace lucene
