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

module lucene_core:field_token_streams.impl;

import :field_token_streams;

import :attribute_source;
import :attributes;
import :error;
import :string_helper;
import :token_stream;
import :types;

import std;

namespace lucene {

Result<uint32_t> utf16_length(std::string_view text) {
    uint64_t length = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t num_bytes = 0;
        auto code_point = StringHelper::utf8_code_point_at(text, pos, num_bytes);
        if (!code_point.has_value()) {
            return std::unexpected(std::move(code_point.error()));
        }
        if (num_bytes == 0) {
            return err(LuceneError::illegal_argument("the string is not valid UTF-8"));
        }
        length += code_point.value() > 0xFFFF ? 2 : 1;
        pos += num_bytes;
    }
    if (length > std::numeric_limits<uint32_t>::max()) {
        return err(LuceneError::illegal_argument("string UTF-16 length exceeds the offset range"));
    }
    return static_cast<uint32_t>(length);
}

// ---------------------------------------------------------------------------
// StringTokenStream
// ---------------------------------------------------------------------------

VoidResult StringTokenStream::set_value(std::string_view value) {
    auto length = utf16_length(value);
    if (!length.has_value()) {
        return std::unexpected(std::move(length.error()));
    }
    end_offset_ = length.value();
    value_.assign(value);
    has_value_ = true;
    return {};
}

VoidResult StringTokenStream::reset() {
    used_ = false;
    return {};
}

Result<bool> StringTokenStream::increment_token() {
    if (used_) {
        return false;
    }
    attributes_.clear_attributes();
    if (!has_value_) {
        return err(LuceneError::illegal_argument("set_value() was not called"));
    }
    attributes_.add_attribute<CharTermAttribute>()->set_text(value_);
    attributes_.add_attribute<OffsetAttribute>()->set_offset(0, end_offset_);
    attributes_.add_attribute<PositionIncrementAttribute>()->set_position_increment(1);
    used_ = true;
    return true;
}

VoidResult StringTokenStream::end() {
    if (auto ended = TokenStream::end(); !ended.has_value()) {
        return ended;
    }
    if (!has_value_) {
        return err(LuceneError::illegal_state("StringTokenStream value is not set"));
    }
    // The reference implementation's `default_end` clears the attributes first, then the final offset
    // is written on the cleared source.
    attributes_.add_attribute<OffsetAttribute>()->set_offset(end_offset_, end_offset_);
    return {};
}

// ---------------------------------------------------------------------------
// BinaryTokenStream
// ---------------------------------------------------------------------------

void BinaryTokenStream::set_value(std::span<const uint8_t> value) {
    value_.assign(value.begin(), value.end());
    used_ = false;
}

Result<bool> BinaryTokenStream::increment_token() {
    if (used_) {
        return false;
    }
    attributes_.clear_attributes();
    attributes_.add_attribute<BytesTermAttribute>()->set_bytes(std::span<const uint8_t>(value_));
    attributes_.add_attribute<PositionIncrementAttribute>()->set_position_increment(1);
    used_ = true;
    return true;
}

} // namespace lucene
