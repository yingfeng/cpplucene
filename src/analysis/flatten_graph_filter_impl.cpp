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

module lucene_core:flatten_graph_filter.impl;

import :flatten_graph_filter;

import :attribute_source;
import :attributes;
import :rolling_buffer;
import :token_stream;
import :types;

import std;

namespace lucene {

namespace {

void erase_first(std::vector<int32_t> &values, int32_t value) {
    const auto found = std::find(values.begin(), values.end(), value);
    if (found != values.end()) {
        values.erase(found);
    }
}

} // namespace

FlattenGraphFilter::FlattenGraphFilter(std::unique_ptr<TokenStream> input) : TokenFilter(std::move(input)) {
    pos_inc_ = attributes().add_attribute<PositionIncrementAttribute>();
    pos_len_ = attributes().add_attribute<PositionLengthAttribute>();
    offset_ = attributes().add_attribute<OffsetAttribute>();
}

VoidResult FlattenGraphFilter::reset() {
    auto result = TokenFilter::reset();
    if (!result.has_value()) {
        return result;
    }
    input_from_ = -1;
    input_nodes_.reset();
    InputNode &in = input_nodes_.get(0);
    in.node = 0;
    in.output_node = 0;

    output_nodes_.reset();
    OutputNode &out = output_nodes_.get(0);
    out.node = 0;
    out.input_nodes.push_back(0);
    out.start_offset = 0;

    output_from_ = 0;
    last_output_from_ = -1;
    done_ = false;
    final_pos_inc_ = -1;
    final_offset_ = -1;
    last_start_offset_ = 0;
    max_lookahead_used_ = 0;
    return {};
}

void FlattenGraphFilter::free_before(OutputNode &output) {
    ++output_from_;
    // Free from the *minimum* input node: alternate paths may appear later in the
    // output than their own tokens, so anything above the minimum may still be in use.
    int32_t boundary = std::numeric_limits<int32_t>::max();
    for (const int32_t id : output.input_nodes) {
        boundary = std::min(boundary, id);
    }
    if (boundary == std::numeric_limits<int32_t>::max()) {
        boundary = input_from_ + 1;
    }
    input_nodes_.free_before(boundary);
    output_nodes_.free_before(output_from_);
}

FlattenGraphFilter::OutputNode &FlattenGraphFilter::recover_from_hole(InputNode &src, int32_t start_offset, int32_t pos_inc) {
    // The "from" node of this token was never seen as a "to" node, which only
    // happens just after crossing a hole.  Jump the output node id instead of
    // dropping the token.
    src.node = input_from_;

    int32_t out_index;
    const int32_t previous_input_from = input_from_ - pos_inc;
    if (previous_input_from >= 0) {
        InputNode &offset_src = input_nodes_.get(previous_input_from);
        if (offset_src.min_to_node < input_from_) {
            // There is a possible path to this node: place it one position off it,
            // keeping a gap of exactly one so it does not get disconnected.
            out_index = input_nodes_.get(offset_src.min_to_node).output_node + 1;
        } else {
            out_index = output_nodes_.max_pos();
        }
    } else {
        // The first token of the stream is a hole: nothing to increment from.
        out_index = output_nodes_.max_pos() + 1;
    }

    OutputNode &out_src = output_nodes_.get(out_index);
    src.output_node = out_index;
    if (out_src.node == -1) {
        out_src.node = src.output_node;
        out_src.start_offset = start_offset;
    } else {
        out_src.start_offset = std::max(start_offset, out_src.start_offset);
    }
    out_src.input_nodes.push_back(input_from_);
    return out_src;
}

bool FlattenGraphFilter::release_buffered_token() {
    // The loop only retries when an output node has no tokens leaving it (a hole).
    while (output_from_ < output_nodes_.max_pos()) {
        OutputNode &output = output_nodes_.get(output_from_);
        if (output.input_nodes.empty()) {
            // No token arrived here, which happens for the first node after a hole.
            ++output_from_;
            continue;
        }

        int32_t max_to_node = -1;
        for (const int32_t id : output.input_nodes) {
            max_to_node = std::max(max_to_node, input_nodes_.get(id).max_to_node);
        }

        if (max_to_node > input_from_ && !done_) {
            // Not frozen yet: we may still merge more input positions into it.
            return false;
        }

        InputNode &input_node = input_nodes_.get(output.input_nodes[static_cast<size_t>(output.next_out)]);
        if (done_ && input_node.tokens.empty() && output_from_ >= output_nodes_.max_pos()) {
            return false;
        }
        if (input_node.tokens.empty()) {
            // Hole destination nodes are never merged (they get their own output
            // position) and never have arriving tokens, so skip them without
            // freeing the input nodes yet.
            if (output.input_nodes.size() > 1) {
                ++output.next_out;
                if (output.next_out < static_cast<int32_t>(output.input_nodes.size())) {
                    continue;
                }
            }
            // Do not free from a hole source: later output nodes may still point
            // at earlier input nodes.
            free_before(output);
            continue;
        }

        attributes().restore_state(input_node.tokens[static_cast<size_t>(input_node.next_out)]);

        // Correct the position increment.
        pos_inc_->set_position_increment(static_cast<uint32_t>(output_from_ - last_output_from_));
        const int32_t to_input_node_id = input_node.node + static_cast<int32_t>(pos_len_->position_length());
        InputNode &to_input_node = input_nodes_.get(to_input_node_id);

        // Correct the position length.
        pos_len_->set_position_length(static_cast<uint32_t>(to_input_node.output_node - output_from_));
        last_output_from_ = output_from_;
        ++input_node.next_out;

        OutputNode &output_end_node = output_nodes_.get(to_input_node.output_node);

        // Correct the offsets.  The start may only move forward (a replacement
        // with more tokens than the input would otherwise run backwards), the end
        // is the end offset of the node the token arrives at.
        const int32_t start_offset = std::max(last_start_offset_, output.start_offset);
        const int32_t end_offset = std::max(start_offset, output_end_node.end_offset);
        offset_->set_offset(static_cast<uint32_t>(start_offset), static_cast<uint32_t>(end_offset));
        last_start_offset_ = start_offset;

        if (input_node.next_out == static_cast<int32_t>(input_node.tokens.size())) {
            ++output.next_out;
            if (output.next_out == static_cast<int32_t>(output.input_nodes.size())) {
                free_before(output);
            }
        }
        return true;
    }
    return false;
}

Result<bool> FlattenGraphFilter::increment_token() {
    while (true) {
        if (release_buffered_token()) {
            return true;
        }
        if (done_) {
            return false;
        }

        auto more = input_->increment_token();
        if (!more.has_value()) {
            return std::unexpected(std::move(more.error()));
        }
        if (!more.value()) {
            auto ended = input_->end();
            if (!ended.has_value()) {
                return std::unexpected(std::move(ended.error()));
            }
            final_pos_inc_ = static_cast<int32_t>(pos_inc_->position_increment());
            final_offset_ = static_cast<int32_t>(offset_->end_offset());
            done_ = true;
            // Don't return false: the buffered tokens must be forced out first.
            continue;
        }

        const int32_t position_increment = static_cast<int32_t>(pos_inc_->position_increment());
        input_from_ += position_increment;

        const int32_t start_offset = static_cast<int32_t>(offset_->start_offset());
        const int32_t end_offset = static_cast<int32_t>(offset_->end_offset());
        const int32_t input_to = input_from_ + static_cast<int32_t>(pos_len_->position_length());

        InputNode &src = input_nodes_.get(input_from_);
        if (src.node == -1) {
            recover_from_hole(src, start_offset, position_increment);
        } else {
            OutputNode *out_src = &output_nodes_.get(src.output_node);
            // A position increment crossing a hole makes the long edge point too
            // far back; fix it unless an earlier long edge already did.
            if (position_increment > 1 &&
                src.output_node - input_nodes_.get(input_from_ - position_increment).output_node <= 1 &&
                input_nodes_.get(input_from_ - position_increment).min_to_node != input_from_) {
                const int32_t previous_end_offset = out_src->end_offset;
                erase_first(out_src->input_nodes, input_from_);
                src.output_node = -1;
                out_src = &recover_from_hole(src, start_offset, position_increment);
                out_src->end_offset = previous_end_offset;
            }

            if (out_src->start_offset == -1 || start_offset > out_src->start_offset) {
                // "Shrink wrap" the offsets so the most restrictive original wins.
                out_src->start_offset = std::max(start_offset, out_src->start_offset);
            }
        }

        src.tokens.push_back(attributes().capture_state());
        src.max_to_node = std::max(src.max_to_node, input_to);
        src.min_to_node = std::min(src.min_to_node, input_to);
        max_lookahead_used_ =
            std::max(max_lookahead_used_, static_cast<int32_t>(input_nodes_.buffer_size()));

        InputNode &dest = input_nodes_.get(input_to);
        if (dest.node == -1) {
            dest.node = input_to;
        }

        // Output nodes are always numbered sequentially.
        const int32_t output_end_node = src.output_node + 1;
        if (output_end_node > dest.output_node) {
            if (dest.output_node != -1) {
                erase_first(output_nodes_.get(dest.output_node).input_nodes, input_to);
            }
            output_nodes_.get(output_end_node).input_nodes.push_back(input_to);
            dest.output_node = output_end_node;
        }

        OutputNode &out_dest = output_nodes_.get(dest.output_node);
        if (out_dest.end_offset == -1 || end_offset < out_dest.end_offset) {
            out_dest.end_offset = end_offset;
        }
    }
}

VoidResult FlattenGraphFilter::end() {
    if (!done_) {
        auto result = TokenFilter::end();
        if (!result.has_value()) {
            return result;
        }
    }
    attributes().clear_attributes();
    if (done_) {
        // `increment_token` already ended the input chain and recorded the final
        // position increment and offset.
        pos_inc_->set_position_increment(static_cast<uint32_t>(final_pos_inc_));
        offset_->set_offset(static_cast<uint32_t>(final_offset_), static_cast<uint32_t>(final_offset_));
        return {};
    }
    return TokenFilter::end();
}

} // namespace lucene
