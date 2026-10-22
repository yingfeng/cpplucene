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

module lucene_core:bytes_ref.impl;

import :bytes_ref;
import :core_helper;
import :string_helper;

import :types;
import :debug;
import std;

namespace lucene {

BytesRef::BytesRef(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)), offset_(0), length_(bytes_.size()) {}

BytesRef BytesRef::from_string(std::string_view text) {
    std::vector<uint8_t> bytes(text.begin(), text.end());
    const size_t len = bytes.size();
    BytesRef ref;
    ref.bytes_ = std::move(bytes);
    ref.offset_ = 0;
    ref.length_ = len;
    return ref;
}

BytesRef BytesRef::from_slice(std::vector<uint8_t> bytes, size_t offset, size_t length) {
    debug_check(offset <= bytes.size() && length <= bytes.size() && offset + length <= bytes.size());
    BytesRef ref;
    ref.bytes_ = std::move(bytes);
    ref.offset_ = offset;
    ref.length_ = length;
    return ref;
}

Result<BytesRef> BytesRef::deep_copy_of(const BytesRef &other) {
    if (auto check = CoreHelper::check_from_index_size(other.offset_, other.length_, other.bytes_.size()); !check) {
        return std::unexpected(std::move(check.error()));
    }
    return BytesRef(std::vector<uint8_t>(other.as_slice().begin(), other.as_slice().end()));
}

void BytesRef::copy_from_slice(std::span<const uint8_t> bytes) {
    bytes_.assign(bytes.begin(), bytes.end());
    offset_ = 0;
    length_ = bytes.size();
}

int32_t BytesRef::compare_to(std::span<const uint8_t> other) const {
    const auto self = as_slice();
    const size_t common = std::min(self.size(), other.size());
    if (common != 0) {
        const int cmp = std::memcmp(self.data(), other.data(), common);
        if (cmp != 0) {
            return cmp < 0 ? -1 : 1;
        }
    }
    if (self.size() == other.size()) {
        return 0;
    }
    return self.size() < other.size() ? -1 : 1;
}

std::strong_ordering BytesRef::operator<=>(const BytesRef &other) const {
    const int32_t cmp = compare_to(other.as_slice());
    if (cmp < 0) {
        return std::strong_ordering::less;
    }
    if (cmp > 0) {
        return std::strong_ordering::greater;
    }
    return std::strong_ordering::equal;
}

VoidResult BytesRef::is_valid() const {
    if (length_ > bytes_.size()) {
        return err(LuceneError::illegal_state(
            std::format("length is out of bounds: {},bytes.length= {}", length_, bytes_.size())));
    }
    if (offset_ > bytes_.size()) {
        return err(LuceneError::illegal_state(
            std::format("offset out of bounds: {},bytes.length= {}", offset_, bytes_.size())));
    }
    if (offset_ + length_ > bytes_.size()) {
        return err(LuceneError::illegal_state(
            std::format("offset+length out of bounds: offset={},length={},bytes.length= {}",
                        offset_,
                        length_,
                        bytes_.size())));
    }
    return {};
}

Result<std::string> BytesRef::utf8_to_string() const {
    if (auto check = CoreHelper::check_from_index_size(offset_, length_, bytes_.size()); !check) {
        return std::unexpected(std::move(check.error()));
    }
    const auto slice = as_slice();
    // Strict UTF-8 validation, mirroring `std::str::from_utf8`.
    size_t i = 0;
    while (i < slice.size()) {
        const uint8_t lead = slice[i];
        size_t continue_bytes = 0;
        uint32_t codepoint = 0;
        if (lead < 0x80) {
            ++i;
            continue;
        }
        if ((lead & 0xE0) == 0xC0) {
            continue_bytes = 1;
            codepoint = lead & 0x1Fu;
        } else if ((lead & 0xF0) == 0xE0) {
            continue_bytes = 2;
            codepoint = lead & 0x0Fu;
        } else if ((lead & 0xF8) == 0xF0) {
            continue_bytes = 3;
            codepoint = lead & 0x07u;
        } else {
            return err(LuceneError::illegal_argument("invalid utf8: unexpected leading byte"));
        }
        if (i + continue_bytes >= slice.size()) {
            return err(LuceneError::illegal_argument("invalid utf8: truncated sequence"));
        }
        for (size_t k = 1; k <= continue_bytes; ++k) {
            const uint8_t next = slice[i + k];
            if ((next & 0xC0) != 0x80) {
                return err(LuceneError::illegal_argument("invalid utf8: invalid continuation byte"));
            }
            codepoint = (codepoint << 6) | (next & 0x3Fu);
        }
        i += continue_bytes + 1;
        // Reject overlong encodings, surrogates and out-of-range code points.
        static constexpr uint32_t kMinCodepoint[4] = {0, 0x80, 0x800, 0x10000};
        if (codepoint < kMinCodepoint[continue_bytes] || codepoint > 0x10FFFF ||
            (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            return err(LuceneError::illegal_argument("invalid utf8: invalid code point"));
        }
    }
    return std::string(reinterpret_cast<const char *>(slice.data()), slice.size());
}

std::vector<uint8_t> BytesRef::take_bytes() {
    auto result = std::move(bytes_);
    bytes_.clear();
    offset_ = 0;
    length_ = 0;
    return result;
}

int32_t BytesRef::hash_code() const { return StringHelper::murmurhash3_x86_32(as_slice(), StringHelper::good_fast_hash_seed()); }

std::string BytesRef::to_display_string() const {
    static constexpr char kHex[] = "0123456789abcdef";
    const auto slice = as_slice();
    std::string result;
    result.reserve(slice.size() * 3 + 2);
    result.push_back('[');
    for (size_t i = 0; i < slice.size(); ++i) {
        if (i > 0) {
            result.push_back(' ');
        }
        result.push_back(kHex[slice[i] >> 4]);
        result.push_back(kHex[slice[i] & 0x0f]);
    }
    result.push_back(']');
    return result;
}

} // namespace lucene
