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

module clucene:bytes_ref_builder.impl;

import :bytes_ref_builder;
import :bytes_ref;
import :array_util;
import :core_helper;

import :types;
import :debug;
import std;

namespace clucene {

uint8_t BytesRefBuilder::byte_at(size_t index) const {
    debug_check(index < bytes_ref_.bytes().size());
    return bytes_ref_.bytes()[index];
}

void BytesRefBuilder::set_byte_at(size_t index, uint8_t value) {
    debug_check(index < bytes_ref_.bytes().size());
    bytes_ref_.bytes()[index] = value;
}

VoidResult BytesRefBuilder::grow(size_t capacity) {
    return ArrayUtil::grow_with(bytes_ref_.bytes(), capacity, [] { return static_cast<uint8_t>(0); });
}

VoidResult BytesRefBuilder::grow_no_copy(size_t capacity) {
    return ArrayUtil::grow_no_copy(bytes_ref_.bytes(), capacity, [] { return static_cast<uint8_t>(0); });
}

VoidResult BytesRefBuilder::append_byte(uint8_t b) {
    if (auto r = grow(bytes_ref_.length() + 1); !r) {
        return r;
    }
    bytes_ref_.bytes()[bytes_ref_.length()] = b;
    bytes_ref_.set_length(bytes_ref_.length() + 1);
    return {};
}

VoidResult BytesRefBuilder::append(std::span<const uint8_t> bytes) {
    if (auto r = grow(bytes_ref_.length() + bytes.size()); !r) {
        return r;
    }
    const size_t pos = bytes_ref_.length();
    std::copy(bytes.begin(), bytes.end(), bytes_ref_.bytes().begin() + static_cast<std::ptrdiff_t>(pos));
    bytes_ref_.set_length(pos + bytes.size());
    return {};
}

VoidResult BytesRefBuilder::copy_bytes(std::span<const uint8_t> bytes, size_t off, size_t len) {
    debug_check(bytes_ref_.offset() == 0);
    if (auto check = CoreHelper::check_from_index_size(off, len, bytes.size()); !check) {
        return std::unexpected(std::move(check.error()));
    }
    if (auto r = grow_no_copy(len); !r) {
        return r;
    }
    std::copy(bytes.begin() + static_cast<std::ptrdiff_t>(off),
              bytes.begin() + static_cast<std::ptrdiff_t>(off + len),
              bytes_ref_.bytes().begin());
    bytes_ref_.set_length(len);
    bytes_ref_.set_offset(0);
    return {};
}

VoidResult BytesRefBuilder::copy_chars(std::string_view text) {
    return copy_bytes(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size()), 0, text.size());
}

VoidResult BytesRefBuilder::copy_chars(std::span<const char32_t> chars, size_t off, size_t len) {
    if (auto check = CoreHelper::check_from_index_size(off, len, chars.size()); !check) {
        return std::unexpected(std::move(check.error()));
    }
    std::string encoded;
    encoded.reserve(len);
    for (size_t i = off; i < off + len; ++i) {
        const char32_t c = chars[i];
        if (c < 0x80) {
            encoded.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            encoded.push_back(static_cast<char>(0xC0 | (c >> 6)));
            encoded.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            encoded.push_back(static_cast<char>(0xE0 | (c >> 12)));
            encoded.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            encoded.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            encoded.push_back(static_cast<char>(0xF0 | (c >> 18)));
            encoded.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            encoded.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            encoded.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return copy_chars(std::string_view(encoded));
}

BytesRef BytesRefBuilder::get_bytes_owner() { return std::move(bytes_ref_); }

Result<BytesRef> BytesRefBuilder::get_bytes_ref_copy() const { return BytesRef::deep_copy_of(bytes_ref_); }

} // namespace clucene
