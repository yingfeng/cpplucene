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

module clucene:data_output.impl;

import :data_output;
import :data_input;
import :bit_util;

import :types;
import std;

namespace clucene {

namespace {
constexpr size_t COPY_BUFFER_SIZE = 16384;
} // namespace

VoidResult DataOutput::write_int(int32_t i) {
    const uint32_t value = static_cast<uint32_t>(i);
    if (auto r = write_byte(static_cast<uint8_t>(value)); !r) {
        return r;
    }
    if (auto r = write_byte(static_cast<uint8_t>(value >> 8)); !r) {
        return r;
    }
    if (auto r = write_byte(static_cast<uint8_t>(value >> 16)); !r) {
        return r;
    }
    return write_byte(static_cast<uint8_t>(value >> 24));
}

VoidResult DataOutput::write_short(int16_t i) {
    const uint16_t value = static_cast<uint16_t>(i);
    if (auto r = write_byte(static_cast<uint8_t>(value)); !r) {
        return r;
    }
    return write_byte(static_cast<uint8_t>(value >> 8));
}

VoidResult DataOutput::write_long(int64_t i) {
    if (auto r = write_int(static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(i) & 0xFFFFFFFFu))); !r) {
        return r;
    }
    return write_int(static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(i) >> 32)));
}

VoidResult DataOutput::write_vint(int32_t i) {
    uint32_t value = static_cast<uint32_t>(i);
    while ((value & ~0x7Fu) != 0) {
        if (auto r = write_byte(static_cast<uint8_t>((value & 0x7F) | 0x80)); !r) {
            return r;
        }
        value >>= 7;
    }
    return write_byte(static_cast<uint8_t>(value));
}

VoidResult DataOutput::write_zint(int32_t i) { return write_vint(BitUtil::zig_zag_encode_i32(i)); }

VoidResult DataOutput::write_vlong(int64_t i) {
    if (i < 0) {
        return err(LuceneError::illegal_argument(std::format("cannot write negative vLong (got: {})", i)));
    }
    return write_signed_vlong(i);
}

VoidResult DataOutput::write_signed_vlong(int64_t i) {
    uint64_t value = static_cast<uint64_t>(i);
    while ((value & ~0x7Full) != 0) {
        if (auto r = write_byte(static_cast<uint8_t>((value & 0x7F) | 0x80)); !r) {
            return r;
        }
        value >>= 7;
    }
    return write_byte(static_cast<uint8_t>(value));
}

VoidResult DataOutput::write_zlong(int64_t i) { return write_signed_vlong(BitUtil::zig_zag_encode_i64(i)); }

VoidResult DataOutput::write_string(std::string_view s) {
    if (auto r = write_vint(static_cast<int32_t>(s.size())); !r) {
        return r;
    }
    return write_bytes(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(s.data()), s.size()));
}

VoidResult DataOutput::copy_bytes(DataInput &input, size_t num_bytes) {
    size_t left = num_bytes;
    std::vector<uint8_t> buffer(COPY_BUFFER_SIZE);
    while (left > 0) {
        const size_t to_copy = std::min(left, COPY_BUFFER_SIZE);
        auto span = std::span<uint8_t>(buffer.data(), to_copy);
        if (auto r = input.read_bytes(span); !r) {
            return std::unexpected(std::move(r.error()));
        }
        if (auto r = write_bytes(span); !r) {
            return r;
        }
        left -= to_copy;
    }
    return {};
}

VoidResult DataOutput::write_map_of_strings(const std::map<std::string, std::string> &map) {
    if (auto r = write_vint(static_cast<int32_t>(map.size())); !r) {
        return r;
    }
    for (const auto &[key, value] : map) {
        if (auto r = write_string(key); !r) {
            return r;
        }
        if (auto r = write_string(value); !r) {
            return r;
        }
    }
    return {};
}

VoidResult DataOutput::write_set_of_strings(const std::set<std::string> &set) {
    if (auto r = write_vint(static_cast<int32_t>(set.size())); !r) {
        return r;
    }
    for (const auto &value : set) {
        if (auto r = write_string(value); !r) {
            return r;
        }
    }
    return {};
}

// ---------------- ByteArrayDataOutput ----------------

ByteArrayDataOutput::ByteArrayDataOutput(std::span<uint8_t> bytes) : bytes_(bytes), pos_(0), limit_(bytes.size()) {}

ByteArrayDataOutput ByteArrayDataOutput::with_range(std::span<uint8_t> bytes, size_t offset, size_t length) {
    ByteArrayDataOutput output;
    output.bytes_ = bytes;
    output.pos_ = offset;
    output.limit_ = offset + length;
    return output;
}

void ByteArrayDataOutput::reset() { reset_with_range(0, bytes_.size()); }

void ByteArrayDataOutput::reset_with_range(size_t offset, size_t length) noexcept {
    pos_ = offset;
    limit_ = offset + length;
}

VoidResult ByteArrayDataOutput::ensure_room(size_t num_bytes) const {
    if (pos_ + num_bytes > limit_ || pos_ + num_bytes > bytes_.size()) {
        return err(LuceneError::illegal_argument(
            std::format("Write exceeds the allowed limit: pos={}, length={}, limit={}", pos_, num_bytes, limit_)));
    }
    return {};
}

VoidResult ByteArrayDataOutput::write_byte(uint8_t b) {
    if (auto r = ensure_room(1); !r) {
        return r;
    }
    bytes_[pos_++] = b;
    return {};
}

VoidResult ByteArrayDataOutput::write_bytes(std::span<const uint8_t> b, size_t offset, size_t length) {
    if (offset + length > b.size()) {
        return err(LuceneError::illegal_argument(std::format("source out of bounds: offset={} length={} size={}",
                                                             offset,
                                                             length,
                                                             b.size())));
    }
    if (auto r = ensure_room(length); !r) {
        return r;
    }
    std::copy(b.begin() + static_cast<std::ptrdiff_t>(offset),
              b.begin() + static_cast<std::ptrdiff_t>(offset + length),
              bytes_.begin() + static_cast<std::ptrdiff_t>(pos_));
    pos_ += length;
    return {};
}

VoidResult ByteArrayDataOutput::write_int(int32_t i) {
    if (auto r = ensure_room(BitUtil::INT_BYTES); !r) {
        return r;
    }
    BitUtil::set_i32_le(bytes_, pos_, i);
    pos_ += BitUtil::INT_BYTES;
    return {};
}

VoidResult ByteArrayDataOutput::write_short(int16_t i) {
    if (auto r = ensure_room(BitUtil::SHORT_BYTES); !r) {
        return r;
    }
    BitUtil::set_i16_le(bytes_, pos_, i);
    pos_ += BitUtil::SHORT_BYTES;
    return {};
}

VoidResult ByteArrayDataOutput::write_long(int64_t i) {
    if (auto r = ensure_room(BitUtil::LONG_BYTES); !r) {
        return r;
    }
    BitUtil::set_i64_le(bytes_, pos_, i);
    pos_ += BitUtil::LONG_BYTES;
    return {};
}

std::string ByteArrayDataOutput::to_string() const {
    return std::format("ByteArrayDataOutput@{}", static_cast<const void *>(this));
}

} // namespace clucene
