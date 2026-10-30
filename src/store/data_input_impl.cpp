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

module lucene_core:data_input.impl;

import :data_input;
import :bit_util;
import :core_helper;

import :types;
import std;

namespace lucene {

namespace {

/// Default group-varint group decode (see `GroupVIntUtil::read_group_vint_i32`).
Result<int32_t> read_int_in_group(DataInput &input, size_t num_bytes_minus1) {
    switch (num_bytes_minus1) {
        case 0: {
            auto byte = input.read_byte();
            if (!byte) {
                return std::unexpected(std::move(byte.error()));
            }
            return static_cast<int32_t>(byte.value());
        }
        case 1: {
            auto value = input.read_short();
            if (!value) {
                return std::unexpected(std::move(value.error()));
            }
            return static_cast<int32_t>(static_cast<uint16_t>(value.value()));
        }
        case 2: {
            auto short_part = input.read_short();
            if (!short_part) {
                return std::unexpected(std::move(short_part.error()));
            }
            auto byte_part = input.read_byte();
            if (!byte_part) {
                return std::unexpected(std::move(byte_part.error()));
            }
            const uint32_t low = static_cast<uint16_t>(short_part.value());
            const uint32_t high = static_cast<uint32_t>(byte_part.value()) << 16;
            return static_cast<int32_t>(low | high);
        }
        default:
            return input.read_int();
    }
}

} // namespace

// ---------------- DataInput defaults ----------------

VoidResult DataInput::read_bytes(std::span<uint8_t> b, size_t offset, size_t len) {
    if (auto check = CoreHelper::check_from_index_size(offset, len, b.size()); !check) {
        return std::unexpected(std::move(check.error()));
    }
    return read_bytes(b.subspan(offset, len));
}

VoidResult DataInput::read_group_vint(std::span<int32_t> dst) {
    if (dst.size() < 4) {
        return err(LuceneError::illegal_argument("read_group_vint requires room for 4 values"));
    }
    auto flag = read_byte();
    if (!flag) {
        return std::unexpected(std::move(flag.error()));
    }
    const size_t bits = flag.value();
    const size_t n1_minus1 = bits >> 6;
    const size_t n2_minus1 = (bits >> 4) & 0x03u;
    const size_t n3_minus1 = (bits >> 2) & 0x03u;
    const size_t n4_minus1 = bits & 0x03u;

    const size_t counts[4] = {n1_minus1, n2_minus1, n3_minus1, n4_minus1};
    for (size_t i = 0; i < 4; ++i) {
        auto value = read_int_in_group(*this, counts[i]);
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        dst[i] = value.value();
    }
    return {};
}

Result<int16_t> DataInput::read_short() {
    auto b1 = read_byte();
    if (!b1) {
        return std::unexpected(std::move(b1.error()));
    }
    auto b2 = read_byte();
    if (!b2) {
        return std::unexpected(std::move(b2.error()));
    }
    return static_cast<int16_t>(static_cast<uint16_t>(b1.value()) | (static_cast<uint16_t>(b2.value()) << 8));
}

Result<int32_t> DataInput::read_int() {
    uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        auto b = read_byte();
        if (!b) {
            return std::unexpected(std::move(b.error()));
        }
        value |= static_cast<uint32_t>(b.value()) << shift;
    }
    return static_cast<int32_t>(value);
}

Result<int64_t> DataInput::read_long() {
    auto low = read_int();
    if (!low) {
        return std::unexpected(std::move(low.error()));
    }
    auto high = read_int();
    if (!high) {
        return std::unexpected(std::move(high.error()));
    }
    return static_cast<int64_t>((static_cast<uint64_t>(static_cast<uint32_t>(high.value())) << 32) |
                                static_cast<uint32_t>(low.value()));
}

Result<int32_t> DataInput::read_vint() {
    auto first = read_byte();
    if (!first) {
        return std::unexpected(std::move(first.error()));
    }
    int32_t b = static_cast<int32_t>(first.value());
    int32_t i = b & 0x7F;
    int32_t shift = 7;
    while ((b & 0x80) != 0) {
        auto next = read_byte();
        if (!next) {
            return std::unexpected(std::move(next.error()));
        }
        b = static_cast<int32_t>(next.value());
        i |= static_cast<int32_t>(static_cast<uint32_t>(b & 0x7F) << shift);
        shift += 7;
    }
    return i;
}

Result<int32_t> DataInput::read_zint() {
    auto value = read_vint();
    if (!value) {
        return std::unexpected(std::move(value.error()));
    }
    return BitUtil::zig_zag_decode_i32(static_cast<uint32_t>(value.value()));
}

Result<int64_t> DataInput::read_vlong() {
    auto first = read_byte();
    if (!first) {
        return std::unexpected(std::move(first.error()));
    }
    int64_t b = static_cast<int64_t>(first.value());
    int64_t i = b & 0x7F;
    int64_t shift = 7;
    while ((b & 0x80) != 0) {
        auto next = read_byte();
        if (!next) {
            return std::unexpected(std::move(next.error()));
        }
        b = static_cast<int64_t>(next.value());
        i |= static_cast<int64_t>(static_cast<uint64_t>(b & 0x7F) << shift);
        shift += 7;
    }
    return i;
}

Result<int64_t> DataInput::read_zlong() {
    auto value = read_vlong();
    if (!value) {
        return std::unexpected(std::move(value.error()));
    }
    return BitUtil::zig_zag_decode_i64(static_cast<uint64_t>(value.value()));
}

Result<std::vector<uint8_t>> DataInput::get_bytes(size_t len) {
    std::vector<uint8_t> buffer(len);
    if (auto r = read_bytes(buffer); !r) {
        return std::unexpected(std::move(r.error()));
    }
    return buffer;
}

VoidResult DataInput::read_longs(std::span<int64_t> dst, size_t offset, size_t len) {
    if (auto check = CoreHelper::check_from_index_size(offset, len, dst.size()); !check) {
        return std::unexpected(std::move(check.error()));
    }
    for (size_t i = 0; i < len; ++i) {
        auto value = read_long();
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        dst[offset + i] = value.value();
    }
    return {};
}

VoidResult DataInput::read_ints(std::span<int32_t> dst, size_t offset, size_t len) {
    if (auto check = CoreHelper::check_from_index_size(offset, len, dst.size()); !check) {
        return std::unexpected(std::move(check.error()));
    }
    for (size_t i = 0; i < len; ++i) {
        auto value = read_int();
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        dst[offset + i] = value.value();
    }
    return {};
}

VoidResult read_ints_bulk(DataInput &input, std::span<int32_t> dst, size_t offset, size_t len) {
    if (auto check = CoreHelper::check_from_index_size(offset, len, dst.size()); !check) {
        return std::unexpected(std::move(check.error()));
    }
    if (len == 0) {
        return {};
    }
    // The destination is `int32_t` storage, so filling it byte-wise is legal and
    // avoids a scratch buffer; the byte order fixup below is a no-op on the
    // little-endian targets this port builds for.
    const size_t num_bytes = len * sizeof(int32_t);
    auto bytes = std::span<uint8_t>(reinterpret_cast<uint8_t *>(dst.data() + offset), num_bytes);
    if (auto read = input.read_bytes(bytes); !read) {
        return std::unexpected(std::move(read.error()));
    }
    if constexpr (std::endian::native == std::endian::big) {
        for (size_t i = 0; i < len; ++i) {
            const uint32_t v = static_cast<uint32_t>(dst[offset + i]);
            dst[offset + i] = static_cast<int32_t>(((v & 0x000000FFU) << 24) | ((v & 0x0000FF00U) << 8) |
                                                  ((v & 0x00FF0000U) >> 8) | ((v & 0xFF000000U) >> 24));
        }
    }
    return {};
}

Result<int32_t> ByteArrayDataInput::read_vint() {
    int32_t value = 0;
    int32_t shift = 0;
    size_t position = pos_;
    while (position < limit_ && shift < 35) {
        const int32_t byte = bytes_[position++];
        value |= static_cast<int32_t>(static_cast<uint32_t>(byte & 0x7F) << shift);
        if ((byte & 0x80) == 0) {
            pos_ = position;
            return value;
        }
        shift += 7;
    }
    // Malformed or truncated: let the generic decoder report it.
    return DataInput::read_vint();
}

Result<int64_t> ByteArrayDataInput::read_vlong() {
    int64_t value = 0;
    int64_t shift = 0;
    size_t position = pos_;
    while (position < limit_ && shift < 70) {
        const int64_t byte = bytes_[position++];
        value |= (byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            pos_ = position;
            return value;
        }
        shift += 7;
    }
    return DataInput::read_vlong();
}

VoidResult ByteArrayDataInput::read_ints(std::span<int32_t> dst, size_t offset, size_t len) {
    return read_ints_bulk(*this, dst, offset, len);
}

VoidResult DataInput::read_floats(std::span<float> dst, size_t offset, size_t len) {
    if (auto check = CoreHelper::check_from_index_size(offset, len, dst.size()); !check) {
        return std::unexpected(std::move(check.error()));
    }
    for (size_t i = 0; i < len; ++i) {
        auto value = read_int();
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        dst[offset + i] = std::bit_cast<float>(static_cast<uint32_t>(value.value()));
    }
    return {};
}

Result<std::string> DataInput::read_string() {
    auto length = read_vint();
    if (!length) {
        return std::unexpected(std::move(length.error()));
    }
    if (length.value() < 0) {
        return err(LuceneError::illegal_argument(std::format("invalid string length: {}", length.value())));
    }
    auto bytes = get_bytes(static_cast<size_t>(length.value()));
    if (!bytes) {
        return std::unexpected(std::move(bytes.error()));
    }
    return std::string(reinterpret_cast<const char *>(bytes.value().data()), bytes.value().size());
}

Result<std::map<std::string, std::string>> DataInput::read_map_of_strings() {
    auto count = read_vint();
    if (!count) {
        return std::unexpected(std::move(count.error()));
    }
    std::map<std::string, std::string> result;
    for (int32_t i = 0; i < count.value(); ++i) {
        auto key = read_string();
        if (!key) {
            return std::unexpected(std::move(key.error()));
        }
        auto value = read_string();
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        result.emplace(std::move(key.value()), std::move(value.value()));
    }
    return result;
}

Result<std::set<std::string>> DataInput::read_set_of_strings() {
    auto count = read_vint();
    if (!count) {
        return std::unexpected(std::move(count.error()));
    }
    std::set<std::string> result;
    for (int32_t i = 0; i < count.value(); ++i) {
        auto value = read_string();
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        result.emplace(std::move(value.value()));
    }
    return result;
}

// ---------------- ByteArrayDataInput ----------------

ByteArrayDataInput::ByteArrayDataInput(std::span<const uint8_t> bytes) : bytes_(bytes), pos_(0), limit_(bytes.size()) {}

ByteArrayDataInput ByteArrayDataInput::with_range(std::span<const uint8_t> bytes, size_t offset, size_t length) {
    ByteArrayDataInput input;
    input.bytes_ = bytes;
    input.pos_ = offset;
    input.limit_ = offset + length;
    return input;
}

void ByteArrayDataInput::reset_meta(size_t offset, size_t length) {
    pos_ = offset;
    limit_ = offset + length;
}

void ByteArrayDataInput::reset_with_range(std::span<const uint8_t> bytes, size_t offset, size_t length) {
    bytes_ = bytes;
    reset_meta(offset, length);
}

Result<std::span<const uint8_t>> ByteArrayDataInput::slice(size_t len) {
    if (pos_ + len > limit_ || pos_ + len > bytes_.size()) {
        return err(LuceneError::eof(std::format("read past eof: pos={} len={} limit={}", pos_, len, limit_)));
    }
    auto result = bytes_.subspan(pos_, len);
    pos_ += len;
    return result;
}

Result<uint8_t> ByteArrayDataInput::read_byte() {
    if (pos_ >= limit_) {
        return err(LuceneError::eof(std::format("read past eof: pos={} limit={}", pos_, limit_)));
    }
    return bytes_[pos_++];
}

VoidResult ByteArrayDataInput::read_bytes(std::span<uint8_t> b) {
    auto source = slice(b.size());
    if (!source) {
        return std::unexpected(std::move(source.error()));
    }
    std::copy(source.value().begin(), source.value().end(), b.begin());
    return {};
}

VoidResult ByteArrayDataInput::skip_bytes(int64_t num_bytes) {
    if (num_bytes < 0) {
        return err(LuceneError::illegal_argument(std::format("negative skip: {}", num_bytes)));
    }
    const size_t target = pos_ + static_cast<size_t>(num_bytes);
    if (target > limit_) {
        return err(LuceneError::eof(std::format("skip past eof: pos={} skip={} limit={}", pos_, num_bytes, limit_)));
    }
    pos_ = target;
    return {};
}

Result<int16_t> ByteArrayDataInput::read_short() {
    const size_t pos = pos_;
    auto source = slice(BitUtil::SHORT_BYTES);
    if (!source) {
        return std::unexpected(std::move(source.error()));
    }
    return BitUtil::get_i16_le(bytes_, pos);
}

Result<int32_t> ByteArrayDataInput::read_int() {
    const size_t pos = pos_;
    auto source = slice(BitUtil::INT_BYTES);
    if (!source) {
        return std::unexpected(std::move(source.error()));
    }
    return BitUtil::get_i32_le(bytes_, pos);
}

Result<int64_t> ByteArrayDataInput::read_long() {
    const size_t pos = pos_;
    auto source = slice(BitUtil::LONG_BYTES);
    if (!source) {
        return std::unexpected(std::move(source.error()));
    }
    return BitUtil::get_i64_le(bytes_, pos);
}

VoidResult ByteArrayDataInput::read_group_vint(std::span<int32_t> dst) { return DataInput::read_group_vint(dst); }

std::string ByteArrayDataInput::to_string() const {
    return std::format("ByteArrayDataInput@{}", static_cast<const void *>(this));
}

} // namespace lucene
