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

export module lucene_core:word_delimiter_graph_filter;

import :attribute_source;
import :attributes;
import :char_array_set;
import :token_stream;
import :types;
import :word_delimiter;

import std;

namespace lucene {

/// Splits words into subwords and emits them as a token **graph**.
///
/// Port of `WordDelimiterGraphFilter`
/// (`analysis/common/miscellaneous/word_delimiter_graph_filter.rs`).
///
/// Splitting rules: intra-word delimiters (`"Wi-Fi"` -> `"Wi", "Fi"`), case
/// transitions (`"PowerShot"` -> `"Power", "Shot"`), letter/digit transitions
/// (`"SD500"` -> `"SD", "500"`), with leading and trailing delimiters of each
/// subword ignored and a trailing `'s` removed (`"O'Neil's"` -> `"O", "Neil"`).
/// The `kCatenate*` flags additionally emit concatenations of runs of subwords,
/// which is what makes `"wi-fi"`, `"wifi"` and `"WiFi"` all reachable.
///
/// Unlike the deprecated `WordDelimiterFilter` this one records the graph: a
/// concatenation spans several positions and carries a `PositionLengthAttribute`
/// larger than one.  Feed it to [`FlattenGraphFilter`](lucene_core:flatten_graph_filter)
/// before indexing; it cannot consume a graph itself.
///
/// **Deviation:** the term is a UTF-8 `std::string`, so `adjust_internal_offsets`
/// only produces per-part offsets when the incoming offsets are byte offsets of
/// the term (which they are for every tokenizer of this port); otherwise the
/// whole token's offsets are repeated, exactly like the reference does when the
/// term and the offsets disagree.
export class WordDelimiterGraphFilter final : public TokenFilter {
public:
    /// `"PowerShot"` -> `"Power", "Shot"`.
    static constexpr uint32_t kGenerateWordParts = 1;
    /// `"500-42"` -> `"500", "42"`.
    static constexpr uint32_t kGenerateNumberParts = 2;
    /// `"wi-fi"` -> `"wifi"`.
    static constexpr uint32_t kCatenateWords = 4;
    /// `"500-42"` -> `"50042"`.
    static constexpr uint32_t kCatenateNumbers = 8;
    /// `"wi-fi-4000"` -> `"wifi4000"`.
    static constexpr uint32_t kCatenateAll = 16;
    /// `"500-42"` -> `"500", "42", "500-42"`.
    static constexpr uint32_t kPreserveOriginal = 32;
    /// Split on a lower-to-upper transition (`"PowerShot"`).
    static constexpr uint32_t kSplitOnCaseChange = 64;
    /// Split on a letter-to-digit transition (`"SD500"`).
    static constexpr uint32_t kSplitOnNumerics = 128;
    /// Drop a trailing `'s` from every subword.
    static constexpr uint32_t kStemEnglishPossessive = 256;
    /// Leave `KeywordAttribute` tokens untouched.
    static constexpr uint32_t kIgnoreKeywords = 512;

    /// `prot_words` (optional) are terms that are never delimited.
    WordDelimiterGraphFilter(std::unique_ptr<TokenStream> input, uint32_t flags,
                             const CharArraySet *prot_words = nullptr, bool adjust_internal_offsets = false);

    [[nodiscard]] VoidResult reset() override;
    [[nodiscard]] Result<bool> increment_token() override;

    /// Human readable form of `flags`, for `to_string()`/tests.
    [[nodiscard]] static std::string flags_to_string(uint32_t flags);

private:
    /// One buffered output token: a graph position plus the slice of the saved
    /// term to emit (or a synthesized term for a concatenation).
    struct Part {
        int32_t start_pos{0};
        int32_t end_pos{0};
        int32_t start_part{0};
        int32_t end_part{0};
        /// Set for a concatenation; empty means "a slice of the saved term".
        std::optional<std::string> term{};
    };

    /// A run of subwords of the same type that can be concatenated.
    struct Concatenation {
        std::string buffer{};
        int32_t start_part{0};
        int32_t end_part{0};
        int32_t start_pos{0};
        int32_t type{0};
        int32_t subword_count{0};

        [[nodiscard]] bool empty() const noexcept { return buffer.empty(); }
        void append(std::string_view text) {
            buffer.append(text);
            ++subword_count;
        }
        void clear() {
            buffer.clear();
            start_part = 0;
            end_part = 0;
            type = 0;
            subword_count = 0;
        }
    };

    [[nodiscard]] bool has_flag(uint32_t flag) const noexcept { return (flags_ & flag) != 0; }

    void buffer_word_parts();
    void save_state();
    void buffer_part(int32_t start_pos, int32_t end_pos, int32_t start_part, int32_t end_part);
    void buffer_part(std::string term, int32_t start_pos, int32_t end_pos, int32_t start_part, int32_t end_part);
    void flush_concatenation(Concatenation &concat);
    [[nodiscard]] bool should_concatenate(int32_t word_type) const;
    [[nodiscard]] bool should_generate_parts(int32_t word_type) const;
    void concatenate(Concatenation &concatenation);
    void sort_parts(size_t from);

    uint32_t flags_;
    const CharArraySet *prot_words_;
    bool adjust_internal_offsets_;

    WordDelimiterIterator iterator_;
    Concatenation concat_{};
    Concatenation concat_all_{};
    int32_t last_concat_count_{0};

    std::vector<Part> parts_{};
    size_t buffered_pos_{0};

    CharTermAttribute *term_{nullptr};
    KeywordAttribute *keyword_{nullptr};
    OffsetAttribute *offset_{nullptr};
    PositionIncrementAttribute *pos_inc_{nullptr};
    PositionLengthAttribute *pos_len_{nullptr};

    std::string saved_term_{};
    int32_t saved_start_offset_{0};
    int32_t saved_end_offset_{0};
    AttributeSource::State saved_state_{};
    bool has_saved_state_{false};
    int32_t last_start_offset_{0};
    bool adjusting_offsets_{false};
    int32_t accum_pos_inc_{0};
    int32_t word_pos_{0};
};

} // namespace lucene
