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

export module lucene_core:synonym_graph_filter;

import :attribute_source;
import :attributes;
import :rolling_buffer;
import :synonym_map;
import :token_stream;
import :types;

import std;

namespace lucene {

/// Applies [`SynonymMap`] rules while preserving the token graph.
///
/// Port of `SynonymGraphFilter` (`analysis/common/synonym/synonym_graph_filter.rs`).
/// A multi-word alternative is emitted as a *side path*: its words occupy
/// consecutive positions and the last one carries a `PositionLengthAttribute`
/// that spans back to the node where the path started, so `a b c` matched by
/// `a b c -> x y` produces `x` at position `0` and `y` at position `1` with
/// position length `2`, plus the original words when the rule keeps them.
///
/// Use [`FlattenGraphFilter`](lucene_core:flatten_graph_filter) after this filter
/// at index time, and keep the graph at query time.
///
/// **Deviations** from the reference implementation:
///
/// 1. Matching walks the map's word sequences instead of an FST; the longest
///    match wins, exactly like the FST version.
/// 2. There is no "live token fast path": a token that starts no match is
///    buffered and replayed, which changes only the capture statistics
///    (`capture_count`, `max_lookahead_used`), not the emitted tokens.
/// 3. `ignore_case` lower cases with the same code-point lowering the
///    [`SynonymFilter`](lucene_core:synonym_filter) uses.
export class SynonymGraphFilter final : public TokenFilter {
public:
    SynonymGraphFilter(std::unique_ptr<TokenStream> input, const SynonymMap &map, bool ignore_case = false);

    [[nodiscard]] VoidResult reset() override;
    [[nodiscard]] Result<bool> increment_token() override;

    /// How many input tokens were buffered (test/introspection aid).
    [[nodiscard]] int32_t capture_count() const noexcept { return capture_count_; }
    /// The highest lookahead depth reached (test/introspection aid).
    [[nodiscard]] int32_t max_lookahead_used() const noexcept { return max_lookahead_used_; }

private:
    using StatePtr = std::shared_ptr<const AttributeSource::State>;

    struct BufferedInputToken {
        std::string term{};
        StatePtr state{};
        int32_t start_offset{-1};
        int32_t end_offset{-1};

        void reset() {
            term.clear();
            state.reset();
            start_offset = -1;
            end_offset = -1;
        }
    };

    struct BufferedOutputToken {
        /// Null for a synthesized synonym, set for an original input token.
        StatePtr state{};
        std::string term{};
        int32_t start_node{0};
        int32_t end_node{0};
    };

    [[nodiscard]] std::string normalize(std::string_view term) const;
    /// Buffers the current input token into the lookahead buffer.
    void capture();
    /// Scans ahead for a rule match, buffering its output graph when found.
    [[nodiscard]] Result<bool> parse();
    void buffer_output_tokens(const std::vector<std::vector<std::string>> &paths, size_t match_input_length,
                              bool keep_orig);
    void release_buffered_token();

    const SynonymMap *map_;
    bool ignore_case_;

    RollingBuffer<BufferedInputToken> lookahead_{};
    int32_t lookahead_next_read_{0};
    int32_t lookahead_next_write_{0};
    std::deque<BufferedOutputToken> output_buffer_{};

    CharTermAttribute *term_{nullptr};
    OffsetAttribute *offset_{nullptr};
    PositionIncrementAttribute *pos_inc_{nullptr};
    PositionLengthAttribute *pos_len_{nullptr};
    TypeAttribute *type_{nullptr};

    int32_t last_node_out_{-1};
    int32_t next_node_out_{0};
    int32_t match_start_offset_{-1};
    int32_t match_end_offset_{-1};
    int32_t capture_count_{0};
    int32_t max_lookahead_used_{0};
    bool finished_{false};
    bool live_token_{false};
};

} // namespace lucene
