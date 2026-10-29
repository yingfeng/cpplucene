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

module lucene_core:synonym_graph_filter.impl;

import :synonym_graph_filter;

import :attribute_source;
import :attributes;
import :rolling_buffer;
import :string_helper;
import :synonym_map;
import :token_stream;
import :types;
import :unicode_util;

import std;

namespace lucene {

namespace {

/// The type recorded on a synthesized synonym token.
constexpr std::string_view kTypeSynonym = "SYNONYM";

} // namespace

SynonymGraphFilter::SynonymGraphFilter(std::unique_ptr<TokenStream> input, const SynonymMap &map, bool ignore_case)
    : TokenFilter(std::move(input)), map_(&map), ignore_case_(ignore_case) {
    term_ = attributes().add_attribute<CharTermAttribute>();
    offset_ = attributes().add_attribute<OffsetAttribute>();
    pos_inc_ = attributes().add_attribute<PositionIncrementAttribute>();
    pos_len_ = attributes().add_attribute<PositionLengthAttribute>();
    type_ = attributes().add_attribute<TypeAttribute>();
}

std::string SynonymGraphFilter::normalize(std::string_view term) const {
    if (!ignore_case_) {
        return std::string(term);
    }
    const auto code_points = StringHelper::utf8_to_code_points(term);
    if (!code_points.has_value()) {
        return std::string(term);
    }
    std::string lowered;
    lowered.reserve(term.size());
    for (const int32_t code_point : code_points.value()) {
        if (!StringHelper::utf8_append(lowered, static_cast<int32_t>(char_to_lower(static_cast<char32_t>(code_point))))
                 .has_value()) {
            return std::string(term);
        }
    }
    return lowered;
}

VoidResult SynonymGraphFilter::reset() {
    auto result = TokenFilter::reset();
    if (!result.has_value()) {
        return result;
    }
    lookahead_.reset();
    lookahead_next_write_ = 0;
    lookahead_next_read_ = 0;
    capture_count_ = 0;
    last_node_out_ = -1;
    next_node_out_ = 0;
    match_start_offset_ = -1;
    match_end_offset_ = -1;
    finished_ = false;
    live_token_ = false;
    output_buffer_.clear();
    max_lookahead_used_ = 0;
    return {};
}

void SynonymGraphFilter::capture() {
    live_token_ = false;
    BufferedInputToken &token = lookahead_.get(lookahead_next_write_);
    ++lookahead_next_write_;

    token.state = std::make_shared<const AttributeSource::State>(attributes().capture_state());
    token.start_offset = static_cast<int32_t>(offset_->start_offset());
    token.end_offset = static_cast<int32_t>(offset_->end_offset());
    token.term.assign(term_->text());

    ++capture_count_;
    max_lookahead_used_ = std::max(max_lookahead_used_, static_cast<int32_t>(lookahead_.buffer_size()));
}

Result<bool> SynonymGraphFilter::parse() {
    // Holds the longest match seen so far.
    const std::vector<std::vector<std::string>> *match_output = nullptr;
    size_t match_input_length = 0;
    std::vector<std::string> words{};
    match_start_offset_ = -1;

    int32_t lookahead_upto = lookahead_next_read_;
    while (words.size() < map_->max_input_length()) {
        int32_t input_end_offset = -1;
        if (lookahead_upto <= lookahead_.max_pos()) {
            // Still inside the lookahead buffer.
            const BufferedInputToken &token = lookahead_.get(lookahead_upto);
            ++lookahead_upto;
            words.push_back(normalize(token.term));
            input_end_offset = token.end_offset;
            if (match_start_offset_ == -1) {
                match_start_offset_ = token.start_offset;
            }
        } else {
            // The lookahead buffer is used up: pull the next real input token.
            if (finished_) {
                break;
            }
            auto more = input_->increment_token();
            if (!more.has_value()) {
                return std::unexpected(std::move(more.error()));
            }
            if (!more.value()) {
                finished_ = true;
                break;
            }
            live_token_ = true;
            const int32_t start_offset = static_cast<int32_t>(offset_->start_offset());
            input_end_offset = static_cast<int32_t>(offset_->end_offset());
            if (match_start_offset_ == -1) {
                match_start_offset_ = start_offset;
            }
            capture();
            ++lookahead_upto;
            words.push_back(normalize(lookahead_.get(lookahead_upto - 1).term));
        }

        if (const std::vector<std::vector<std::string>> *found = map_->lookup(words);
            found != nullptr && !found->empty()) {
            match_output = found;
            match_input_length = words.size();
            match_end_offset_ = input_end_offset;
        }
    }

    if (match_output == nullptr) {
        return false;
    }

    // The map records the input words as one more alternative when a rule keeps
    // them; they are replayed from their buffered states instead of being
    // synthesized, exactly like the reference implementation's `keepOrig`.
    std::vector<std::vector<std::string>> paths{};
    bool keep_orig = false;
    const std::vector<std::string> matched(words.begin(), words.begin() + static_cast<std::ptrdiff_t>(match_input_length));
    for (const std::vector<std::string> &alternative : *match_output) {
        if (alternative == matched) {
            keep_orig = true;
        } else {
            paths.push_back(alternative);
        }
    }

    buffer_output_tokens(paths, match_input_length, keep_orig);
    lookahead_next_read_ += static_cast<int32_t>(match_input_length);
    lookahead_.free_before(lookahead_next_read_);
    return true;
}

void SynonymGraphFilter::buffer_output_tokens(const std::vector<std::vector<std::string>> &paths,
                                              size_t match_input_length, bool keep_orig) {
    // How many nodes are on all the paths together; the final node closes them.
    int32_t total_path_nodes = 0;
    if (keep_orig) {
        total_path_nodes = static_cast<int32_t>(match_input_length) - 1;
    }
    for (const std::vector<std::string> &path : paths) {
        total_path_nodes += static_cast<int32_t>(path.size()) - 1;
    }

    const int32_t start_node = next_node_out_;
    const int32_t end_node = start_node + total_path_nodes + 1;

    // First fan out the tokens leaving the start node.
    int32_t new_node_count = 0;
    for (const std::vector<std::string> &path : paths) {
        int32_t path_end_node;
        if (path.size() == 1) {
            // A single token output has no intermediate nodes.
            path_end_node = end_node;
        } else {
            path_end_node = next_node_out_ + new_node_count + 1;
            new_node_count += static_cast<int32_t>(path.size()) - 1;
        }
        output_buffer_.push_back(BufferedOutputToken{nullptr, path.front(), start_node, path_end_node});
    }

    // The original tokens must come after the synonyms, else offsets go backwards.
    if (keep_orig) {
        const BufferedInputToken &token = lookahead_.get(lookahead_next_read_);
        const int32_t input_end_node =
            match_input_length == 1 ? end_node : next_node_out_ + new_node_count + 1;
        output_buffer_.push_back(BufferedOutputToken{token.state, token.term, start_node, input_end_node});
    }

    next_node_out_ = end_node;

    // Now the rest of each side path.
    for (size_t path_id = 0; path_id < paths.size(); ++path_id) {
        const std::vector<std::string> &path = paths[path_id];
        if (path.size() > 1) {
            int32_t last_node = output_buffer_[path_id].end_node;
            for (size_t i = 1; i + 1 < path.size(); ++i) {
                output_buffer_.push_back(BufferedOutputToken{nullptr, path[i], last_node, last_node + 1});
                ++last_node;
            }
            output_buffer_.push_back(BufferedOutputToken{nullptr, path.back(), last_node, end_node});
        }
    }

    if (keep_orig && match_input_length > 1) {
        // The original tokens form a side path of their own.
        int32_t last_node = output_buffer_[paths.size()].end_node;
        for (size_t i = 1; i + 1 < match_input_length; ++i) {
            const BufferedInputToken &token =
                lookahead_.get(lookahead_next_read_ + static_cast<int32_t>(i));
            output_buffer_.push_back(BufferedOutputToken{token.state, token.term, last_node, last_node + 1});
            ++last_node;
        }
        const BufferedInputToken &token =
            lookahead_.get(lookahead_next_read_ + static_cast<int32_t>(match_input_length) - 1);
        output_buffer_.push_back(BufferedOutputToken{token.state, token.term, last_node, end_node});
    }
}

void SynonymGraphFilter::release_buffered_token() {
    BufferedOutputToken token = std::move(output_buffer_.front());
    output_buffer_.pop_front();

    if (token.state != nullptr) {
        // An original input token (`keepOrig`): replay its attributes.
        attributes().restore_state(*token.state);
    } else {
        attributes().clear_attributes();
        term_->set_text(token.term);
        offset_->set_offset(static_cast<uint32_t>(match_start_offset_), static_cast<uint32_t>(match_end_offset_));
        type_->set_type(kTypeSynonym);
    }

    pos_inc_->set_position_increment(static_cast<uint32_t>(token.start_node - last_node_out_));
    last_node_out_ = token.start_node;
    pos_len_->set_position_length(static_cast<uint32_t>(token.end_node - token.start_node));
}

Result<bool> SynonymGraphFilter::increment_token() {
    if (!output_buffer_.empty()) {
        // Pending outputs from a previous match.
        release_buffered_token();
        return true;
    }

    auto matched = parse();
    if (!matched.has_value()) {
        return std::unexpected(std::move(matched.error()));
    }
    if (matched.value()) {
        release_buffered_token();
        return true;
    }

    if (lookahead_next_read_ == lookahead_next_write_) {
        // No lookahead left: the fast path of the reference implementation, where
        // the token pulled by `parse` is returned live.
        if (finished_) {
            return false;
        }
        live_token_ = false;
    } else {
        // Replay the buffered lookahead tokens.
        BufferedInputToken &token = lookahead_.get(lookahead_next_read_);
        ++lookahead_next_read_;
        attributes().restore_state(*token.state);
        lookahead_.free_before(lookahead_next_read_);
    }

    last_node_out_ += static_cast<int32_t>(pos_inc_->position_increment());
    next_node_out_ = last_node_out_ + static_cast<int32_t>(pos_len_->position_length());
    return true;
}

} // namespace lucene
