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

module lucene_core:term.impl;

import :term;

import :bytes_ref;
import :error;

import std;

namespace lucene {

Term::Term(std::string field, BytesRef bytes) : field_(std::move(field)), bytes_(std::move(bytes)) {}

Term Term::from_text(std::string field, std::string_view text) {
    return Term(std::move(field), BytesRef::from_string(text));
}

Term Term::from_empty(std::string field) { return Term(std::move(field), BytesRef{}); }

std::string Term::text() const {
    const std::span<const uint8_t> bytes = bytes_.as_slice();
    // Only report text when the bytes are valid UTF-8, like the reference implementation.
    const auto *begin = reinterpret_cast<const char *>(bytes.data());
    const std::string_view view(begin, bytes.size());
    size_t index = 0;
    while (index < view.size()) {
        const auto byte = static_cast<uint8_t>(view[index]);
        size_t length = 0;
        if (byte < 0x80) {
            length = 1;
        } else if ((byte & 0xE0) == 0xC0) {
            length = 2;
        } else if ((byte & 0xF0) == 0xE0) {
            length = 3;
        } else if ((byte & 0xF8) == 0xF0) {
            length = 4;
        } else {
            return bytes_.to_display_string();
        }
        if (index + length > view.size()) {
            return bytes_.to_display_string();
        }
        for (size_t i = 1; i < length; ++i) {
            if ((static_cast<uint8_t>(view[index + i]) & 0xC0) != 0x80) {
                return bytes_.to_display_string();
            }
        }
        index += length;
    }
    return std::string(view);
}

std::string Term::to_string() const { return field_ + ":" + text(); }

int32_t Term::compare_to(const Term &other) const {
    const int32_t field_order = field_.compare(other.field_) < 0 ? -1 : (field_ == other.field_ ? 0 : 1);
    if (field_order != 0) {
        return field_order;
    }
    return bytes_.compare_to(other.bytes_);
}

} // namespace lucene
