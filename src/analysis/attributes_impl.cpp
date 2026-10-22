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

module lucene_core:attributes.impl;

import :attributes;
import :types;

import std;

namespace lucene {

namespace {

/// UTF-8 decoding used by `char_at` and the tokenizers; assumes valid UTF-8.
std::u32string decode_utf8(std::string_view text) {
    std::u32string result;
    result.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        size_t extra = 0;
        char32_t value = 0;
        if (lead < 0x80) {
            result.push_back(lead);
            ++i;
            continue;
        }
        if ((lead & 0xE0) == 0xC0) {
            extra = 1;
            value = lead & 0x1Fu;
        } else if ((lead & 0xF0) == 0xE0) {
            extra = 2;
            value = lead & 0x0Fu;
        } else {
            extra = 3;
            value = lead & 0x07u;
        }
        if (i + extra >= text.size()) {
            break;
        }
        for (size_t k = 1; k <= extra; ++k) {
            value = (value << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
        }
        result.push_back(value);
        i += extra + 1;
    }
    return result;
}

size_t encoded_length(char32_t codepoint) noexcept {
    if (codepoint < 0x80) {
        return 1;
    }
    if (codepoint < 0x800) {
        return 2;
    }
    if (codepoint < 0x10000) {
        return 3;
    }
    return 4;
}

void encode_into(std::vector<char> &out, size_t pos, char32_t codepoint) {
    if (codepoint < 0x80) {
        out[pos] = static_cast<char>(codepoint);
    } else if (codepoint < 0x800) {
        out[pos] = static_cast<char>(0xC0 | (codepoint >> 6));
        out[pos + 1] = static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
        out[pos] = static_cast<char>(0xE0 | (codepoint >> 12));
        out[pos + 1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out[pos + 2] = static_cast<char>(0x80 | (codepoint & 0x3F));
    } else {
        out[pos] = static_cast<char>(0xF0 | (codepoint >> 18));
        out[pos + 1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        out[pos + 2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out[pos + 3] = static_cast<char>(0x80 | (codepoint & 0x3F));
    }
}

} // namespace

// ---------------- CharTermAttribute ----------------

std::unique_ptr<Attribute> CharTermAttribute::clone() const {
    auto copy = std::make_unique<CharTermAttribute>();
    copy->buffer_ = buffer_;
    copy->length_ = length_;
    return copy;
}

void CharTermAttribute::clear() { length_ = 0; }

char32_t CharTermAttribute::char_at(size_t index) const {
    const auto decoded = decode_utf8(text());
    return index < decoded.size() ? decoded[index] : U'\0';
}

void CharTermAttribute::set_text(std::string_view text) {
    resize_buffer(text.size());
    std::copy(text.begin(), text.end(), buffer_.begin());
    length_ = text.size();
}

void CharTermAttribute::append(std::string_view text) {
    resize_buffer(length_ + text.size());
    std::copy(text.begin(), text.end(), buffer_.begin() + static_cast<std::ptrdiff_t>(length_));
    length_ += text.size();
}

void CharTermAttribute::append(char32_t codepoint) {
    const size_t extra = encoded_length(codepoint);
    resize_buffer(length_ + extra);
    encode_into(buffer_, length_, codepoint);
    length_ += extra;
}

void CharTermAttribute::resize_buffer(size_t capacity) {
    if (buffer_.size() < capacity) {
        // Grow by 1/8 like `ArrayUtil::oversize`, minimum 16 bytes.
        size_t new_size = std::max<size_t>(16, buffer_.size());
        while (new_size < capacity) {
            new_size += std::max<size_t>(3, new_size >> 3);
        }
        buffer_.resize(new_size, '\0');
    }
}

std::u32string CharTermAttribute::to_u32string() const { return decode_utf8(text()); }

std::string_view CharTermAttribute::sub_sequence(size_t start, size_t end) const {
    const std::string_view value = text();
    if (start > end || end > value.size()) {
        return {};
    }
    return value.substr(start, end - start);
}

// ---------------- BytesTermAttribute ----------------

std::unique_ptr<Attribute> BytesTermAttribute::clone() const {
    auto copy = std::make_unique<BytesTermAttribute>();
    copy->bytes_ = bytes_;
    return copy;
}

void BytesTermAttribute::clear() { bytes_.clear(); }

void BytesTermAttribute::set_bytes(std::span<const uint8_t> bytes) {
    bytes_.assign(bytes.begin(), bytes.end());
}

// ---------------- simple value attributes ----------------

std::unique_ptr<Attribute> OffsetAttribute::clone() const {
    auto copy = std::make_unique<OffsetAttribute>();
    copy->start_offset_ = start_offset_;
    copy->end_offset_ = end_offset_;
    return copy;
}

void OffsetAttribute::clear() {
    start_offset_ = 0;
    end_offset_ = 0;
}

std::unique_ptr<Attribute> PositionIncrementAttribute::clone() const {
    auto copy = std::make_unique<PositionIncrementAttribute>();
    copy->position_increment_ = position_increment_;
    return copy;
}

void PositionIncrementAttribute::clear() { position_increment_ = 1; }

std::unique_ptr<Attribute> PositionLengthAttribute::clone() const {
    auto copy = std::make_unique<PositionLengthAttribute>();
    copy->position_length_ = position_length_;
    return copy;
}

void PositionLengthAttribute::clear() { position_length_ = 1; }

std::unique_ptr<Attribute> TypeAttribute::clone() const {
    auto copy = std::make_unique<TypeAttribute>();
    copy->type_ = type_;
    return copy;
}

void TypeAttribute::clear() { type_ = "word"; }

std::unique_ptr<Attribute> KeywordAttribute::clone() const {
    auto copy = std::make_unique<KeywordAttribute>();
    copy->keyword_ = keyword_;
    return copy;
}

void KeywordAttribute::clear() { keyword_ = false; }

std::unique_ptr<Attribute> PayloadAttribute::clone() const {
    auto copy = std::make_unique<PayloadAttribute>();
    copy->payload_ = payload_;
    return copy;
}

void PayloadAttribute::clear() { payload_.reset(); }

std::unique_ptr<Attribute> TermFrequencyAttribute::clone() const {
    auto copy = std::make_unique<TermFrequencyAttribute>();
    copy->term_frequency_ = term_frequency_;
    return copy;
}

void TermFrequencyAttribute::clear() { term_frequency_ = 1; }

std::unique_ptr<Attribute> FlagsAttribute::clone() const {
    auto copy = std::make_unique<FlagsAttribute>();
    copy->flags_ = flags_;
    return copy;
}

void FlagsAttribute::clear() { flags_ = 0; }

} // namespace lucene
