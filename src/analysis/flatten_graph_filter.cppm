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

export module lucene_core:flatten_graph_filter;

import :attribute_source;
import :attributes;
import :rolling_buffer;
import :token_stream;
import :types;

import std;

namespace lucene {

/// Turns a graph token stream (side paths, `PositionLengthAttribute > 1`) into a
/// single linear chain.
///
/// Port of `FlattenGraphFilter` (`analysis/common/core/flatten_graph_filter.rs`).
/// An index does not store the position length, so a graph has to be flattened
/// before indexing; at query time the graph should be kept instead.
///
/// The flattening is lossy by construction: every path through the graph is made
/// to touch every node, so it accepts token sequences the graph rejected and
/// rejects some it accepted.
export class FlattenGraphFilter final : public TokenFilter {
public:
    explicit FlattenGraphFilter(std::unique_ptr<TokenStream> input);

    [[nodiscard]] VoidResult reset() override;
    [[nodiscard]] Result<bool> increment_token() override;
    [[nodiscard]] VoidResult end() override;

    /// The highest number of buffered positions reached (test/introspection aid).
    [[nodiscard]] int32_t max_lookahead_used() const noexcept { return max_lookahead_used_; }

private:
    /// Everything leaving one input position.
    struct InputNode {
        std::vector<AttributeSource::State> tokens{};
        /// Assigned input node, or -1 while unassigned.
        int32_t node{-1};
        /// Maximum destination of the arcs leaving here.
        int32_t max_to_node{-1};
        /// Minimum destination of the arcs leaving here (hole detection).
        int32_t min_to_node{std::numeric_limits<int32_t>::max()};
        /// The output position this maps to; only increases.
        int32_t output_node{-1};
        /// Index into `tokens` of the next token to emit.
        int32_t next_out{0};

        void reset() {
            tokens.clear();
            node = -1;
            output_node = -1;
            max_to_node = -1;
            min_to_node = std::numeric_limits<int32_t>::max();
            next_out = 0;
        }
    };

    /// The merged input positions that form one output position.
    struct OutputNode {
        std::vector<int32_t> input_nodes{};
        int32_t node{-1};
        int32_t next_out{0};
        /// Start offset of the tokens leaving this node.
        int32_t start_offset{-1};
        /// End offset of the tokens arriving at this node.
        int32_t end_offset{-1};

        void reset() {
            input_nodes.clear();
            node = -1;
            next_out = 0;
            start_offset = -1;
            end_offset = -1;
        }
    };

    [[nodiscard]] bool release_buffered_token();
    void free_before(OutputNode &output);
    OutputNode &recover_from_hole(InputNode &src, int32_t start_offset, int32_t pos_inc);

    RollingBuffer<InputNode> input_nodes_{};
    RollingBuffer<OutputNode> output_nodes_{};

    PositionIncrementAttribute *pos_inc_{nullptr};
    PositionLengthAttribute *pos_len_{nullptr};
    OffsetAttribute *offset_{nullptr};

    /// Which input node the last seen token left from.
    int32_t input_from_{-1};
    /// The output node currently being released.
    int32_t output_from_{0};
    int32_t last_output_from_{-1};
    int32_t final_offset_{-1};
    int32_t final_pos_inc_{-1};
    int32_t max_lookahead_used_{0};
    int32_t last_start_offset_{0};
    bool done_{false};
};

} // namespace lucene
