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

export module lucene_core:porter_stemmer;

import :types;

import std;

namespace lucene {

/// The Porter stemming algorithm, as published by Martin Porter in 1980.
///
/// Port of `PorterStemmer` (`analysis/common/en/porter_stemmer.rs`), which itself
/// carries the reference implementation's two later fixes (the `k == k0` guard in
/// step 3 and step 5, and the `j >= 0` guard before `-ion`).  It works on code
/// points, so a term that is not ASCII — nobody stems Chinese with Porter — is
/// returned unchanged: none of the suffixes it strips are non-ASCII.
///
/// One instance keeps scratch state and is therefore **not** thread safe; a
/// [`PorterStemFilter`](lucene_core:porter_stem_filter) owns one instance and a
/// filter chain is used by one thread at a time.
export class PorterStemmer {
public:
    /// Stems `word`, returning the input when nothing was stripped.
    [[nodiscard]] std::string stem(std::string_view word);

private:
    // The algorithm, state in `b_` between `k0_` and `k_` (inclusive).
    [[nodiscard]] bool cons(int32_t index) const;
    [[nodiscard]] int32_t measure() const;
    [[nodiscard]] bool vowel_in_stem() const;
    [[nodiscard]] bool double_consonant(int32_t index) const;
    [[nodiscard]] bool consonant_vowel_consonant(int32_t index) const;
    /// Whether the buffer ends with `suffix`; on a match `j_` is left at the
    /// character before the suffix, like the reference implementation.
    [[nodiscard]] bool ends(std::string_view suffix);
    /// Replaces everything after `j_` with `suffix`.
    void set_to(std::string_view suffix);
    /// [`set_to`] when the stem before the suffix measures more than zero.
    void replace_if_measure_positive(std::string_view suffix);
    void step1();
    void step2();
    void step3();
    void step4();
    void step5();
    void step6();
    /// Runs the steps; returns whether the word changed.
    [[nodiscard]] bool run(int32_t k0);

    std::vector<char32_t> buffer_;
    int32_t k_{0};
    int32_t j_{0};
    int32_t k0_{0};
    int32_t length_{0};
    bool dirty_{false};
};

} // namespace lucene
