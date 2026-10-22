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

module lucene_core:tokenizer.impl;

import :tokenizer;
import :types;
import :error;
import :attribute_source;
import :attributes;
import :token_stream;
import :unicode_util;

import std;

namespace lucene {

namespace {

/// Decodes UTF-8 and records the byte offset of every code point.
void decode(std::string_view input, std::u32string &code_points, std::vector<uint32_t> &byte_offsets) {
    code_points.clear();
    byte_offsets.clear();
    code_points.reserve(input.size());
    byte_offsets.reserve(input.size() + 1);

    size_t i = 0;
    while (i < input.size()) {
        const auto lead = static_cast<unsigned char>(input[i]);
        if (lead < 0x80) {
            byte_offsets.push_back(static_cast<uint32_t>(i));
            code_points.push_back(lead);
            ++i;
            continue;
        }
        size_t extra = (lead & 0xE0) == 0xC0 ? 1 : (lead & 0xF0) == 0xE0 ? 2 : 3;
        char32_t value = extra == 1 ? (lead & 0x1Fu) : extra == 2 ? (lead & 0x0Fu) : (lead & 0x07u);
        if (i + extra >= input.size()) {
            // Truncated sequence: treat the remaining bytes as opaque.
            byte_offsets.push_back(static_cast<uint32_t>(i));
            code_points.push_back(0xFFFD);
            i = input.size();
            break;
        }
        for (size_t k = 1; k <= extra; ++k) {
            value = (value << 6) | (static_cast<unsigned char>(input[i + k]) & 0x3Fu);
        }
        byte_offsets.push_back(static_cast<uint32_t>(i));
        code_points.push_back(value);
        i += extra + 1;
    }
    byte_offsets.push_back(static_cast<uint32_t>(input.size()));
}

} // namespace

Tokenizer::Tokenizer() {
    attributes_.add_attribute<CharTermAttribute>();
    attributes_.add_attribute<OffsetAttribute>();
    attributes_.add_attribute<PositionIncrementAttribute>();
    attributes_.add_attribute<PositionLengthAttribute>();
    attributes_.add_attribute<TypeAttribute>();
}

VoidResult Tokenizer::set_reader(std::string_view input) {
    input_ = std::string(input);
    decode(input_, code_points_, byte_offsets_);
    position_ = 0;
    return {};
}

VoidResult Tokenizer::reset() {
    position_ = 0;
    attributes_.end_attributes();
    return {};
}

uint32_t Tokenizer::byte_offset_at(size_t index) const noexcept {
    if (index >= byte_offsets_.size()) {
        return byte_offsets_.empty() ? 0 : byte_offsets_.back();
    }
    return byte_offsets_[index];
}

std::string_view Tokenizer::text_of(size_t start, size_t end) const noexcept {
    const uint32_t begin = byte_offset_at(start);
    const uint32_t finish = byte_offset_at(end);
    return std::string_view(input_).substr(begin, finish - begin);
}

bool Tokenizer::emit_token(size_t start, size_t end) {
    auto *term = attributes_.add_attribute<CharTermAttribute>();
    auto *offset = attributes_.add_attribute<OffsetAttribute>();
    auto *position_increment = attributes_.add_attribute<PositionIncrementAttribute>();

    term->set_text(text_of(start, end));
    offset->set_offset(byte_offset_at(start), byte_offset_at(end));
    position_increment->set_position_increment(1);
    return true;
}

// ---------------- CharTokenizer ----------------

Result<bool> CharTokenizer::increment_token() {
    const size_t count = code_point_count();
    size_t start = position_;
    while (start < count && !is_token_char(code_point_at(start))) {
        ++start;
    }
    if (start >= count) {
        position_ = count;
        return false;
    }
    size_t end = start;
    while (end < count && is_token_char(code_point_at(end))) {
        ++end;
    }
    position_ = end;
    return emit_token(start, end);
}

// ---------------- KeywordTokenizer ----------------

Result<bool> KeywordTokenizer::increment_token() {
    if (position_ == 0) {
        position_ = 1;
        return emit_token(0, code_point_count());
    }
    return false;
}

} // namespace lucene
