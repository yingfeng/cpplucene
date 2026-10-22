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

module lucene_core:group_vint_util.impl;

import :group_vint_util;

import :types;
import std;

import :error;
import :bit_util;
import :data_input;
import :data_output;

namespace lucene {

size_t GroupVIntUtil::num_bytes(int32_t v) {
    // `| 1` ensures the result is 1 when `v == 0`.
    return BitUtil::INT_BYTES - (std::countl_zero(static_cast<uint32_t>(v) | 1u) / 8);
}

Result<int32_t> GroupVIntUtil::to_int(int64_t value) {
    if (static_cast<uint64_t>(value) > 0xFFFF'FFFFull) {
        return err(LuceneError::number_overflow(std::format("value: {} is out of range to be converted to int32_t", value)));
    }
    return static_cast<int32_t>(value);
}

Result<int32_t> GroupVIntUtil::read_int_in_group(DataInput &input, size_t num_bytes_minus1) {
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

VoidResult GroupVIntUtil::read_group_vint_i64(DataInput &input, std::span<int64_t> dst, size_t offset) {
    auto flag = input.read_byte();
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
        auto value = read_int_in_group(input, counts[i]);
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        // Widen as unsigned so that negative int32_t bit patterns stay positive.
        dst[offset + i] = static_cast<int64_t>(static_cast<uint32_t>(value.value()));
    }
    return {};
}

VoidResult GroupVIntUtil::read_group_vint_i32(DataInput &input, std::span<int32_t> dst, size_t offset) {
    auto flag = input.read_byte();
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
        auto value = read_int_in_group(input, counts[i]);
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        dst[offset + i] = value.value();
    }
    return {};
}

VoidResult GroupVIntUtil::read_group_vints_i64(DataInput &input, std::span<int64_t> dst, size_t limit) {
    if (dst.size() < limit) {
        return err(LuceneError::illegal_argument("dst is too small for the requested limit"));
    }
    size_t i = 0;
    while (i + 4 <= limit) {
        auto result = read_group_vint_i64(input, dst, i);
        if (!result) {
            return result;
        }
        i += 4;
    }
    while (i < limit) {
        auto vint = input.read_vint();
        if (!vint) {
            return std::unexpected(std::move(vint.error()));
        }
        dst[i] = static_cast<int64_t>(static_cast<uint32_t>(vint.value()));
        ++i;
    }
    return {};
}

VoidResult GroupVIntUtil::read_group_vints_i32(DataInput &input, std::span<int32_t> dst, size_t limit) {
    if (dst.size() < limit) {
        return err(LuceneError::illegal_argument("dst is too small for the requested limit"));
    }
    size_t i = 0;
    while (i + 4 <= limit) {
        // Delegate to the `DataInput` fast path, which is allowed to override it.
        auto result = input.read_group_vint(dst.subspan(i, 4));
        if (!result) {
            return result;
        }
        i += 4;
    }
    while (i < limit) {
        auto vint = input.read_vint();
        if (!vint) {
            return std::unexpected(std::move(vint.error()));
        }
        dst[i] = vint.value();
        ++i;
    }
    return {};
}

Result<size_t> GroupVIntUtil::read_group_vint_i64_with_reader(DataInput &input,
                                                             IntReader &reader,
                                                             uint64_t remaining,
                                                             size_t pos,
                                                             std::span<int64_t> dst,
                                                             size_t offset) {
    if (remaining < MAX_LENGTH_PER_GROUP) {
        auto result = read_group_vint_i64(input, dst, offset);
        if (!result) {
            return std::unexpected(std::move(result.error()));
        }
        return 0;
    }

    auto flag = input.read_byte();
    if (!flag) {
        return std::unexpected(std::move(flag.error()));
    }
    const size_t bits = flag.value();
    ++pos; // the flag byte itself is not part of the returned length.
    const size_t pos_start = pos;

    const size_t counts[4] = {bits >> 6, (bits >> 4) & 0x03u, (bits >> 2) & 0x03u, bits & 0x03u};
    for (size_t i = 0; i < 4; ++i) {
        // This code path has fewer conditionals than `read_int_in_group` and
        // tends to be significantly faster: one random-access read per value.
        auto value = reader.read(pos);
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        dst[offset + i] = static_cast<int64_t>(static_cast<uint64_t>(static_cast<uint32_t>(value.value())) & LONG_MASKS[counts[i]]);
        pos += 1 + counts[i];
    }
    return pos - pos_start;
}

Result<size_t> GroupVIntUtil::read_group_vint_i32_with_reader(DataInput &input,
                                                             IntReader &reader,
                                                             size_t remaining,
                                                             size_t pos,
                                                             std::span<int32_t> dst,
                                                             size_t offset) {
    if (remaining < MAX_LENGTH_PER_GROUP) {
        auto result = read_group_vint_i32(input, dst, offset);
        if (!result) {
            return std::unexpected(std::move(result.error()));
        }
        return 0;
    }

    auto flag = input.read_byte();
    if (!flag) {
        return std::unexpected(std::move(flag.error()));
    }
    const size_t bits = flag.value();
    ++pos; // the flag byte itself is not part of the returned length.
    const size_t pos_start = pos;

    const size_t counts[4] = {bits >> 6, (bits >> 4) & 0x03u, (bits >> 2) & 0x03u, bits & 0x03u};
    for (size_t i = 0; i < 4; ++i) {
        auto value = reader.read(pos);
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        dst[offset + i] = value.value() & INT_MASKS[counts[i]];
        pos += 1 + counts[i];
    }
    return pos - pos_start;
}

namespace {

/// Copies the low `count` little-endian bytes of `value` into `scratch` at
/// `write_pos`, then advances `write_pos` by `count`.
void write_int_le(std::span<uint8_t> scratch, size_t &write_pos, int32_t value, size_t count) {
    const uint32_t bits = static_cast<uint32_t>(value);
    for (size_t i = 0; i < count; ++i) {
        scratch[write_pos + i] = static_cast<uint8_t>((bits >> (8 * i)) & 0xFFu);
    }
    write_pos += count;
}

} // namespace

VoidResult GroupVIntUtil::write_group_vints_i64(DataOutput &data_output,
                                                std::span<uint8_t> scratch,
                                                std::span<const int64_t> values,
                                                size_t limit) {
    if (limit > values.size()) {
        return err(LuceneError::illegal_argument("values is too small for the requested limit"));
    }
    if (scratch.size() < MAX_LENGTH_PER_GROUP) {
        return err(LuceneError::illegal_argument("scratch must provide MAX_LENGTH_PER_GROUP bytes"));
    }

    size_t read_pos = 0;
    std::array<int32_t, 4> group{};
    while (limit - read_pos >= 4) {
        for (size_t i = 0; i < 4; ++i) {
            auto narrowed = to_int(values[read_pos + i]);
            if (!narrowed) {
                return std::unexpected(std::move(narrowed.error()));
            }
            group[i] = narrowed.value();
        }
        const size_t n1_minus1 = num_bytes(group[0]);
        const size_t n2_minus1 = num_bytes(group[1]);
        const size_t n3_minus1 = num_bytes(group[2]);
        const size_t n4_minus1 = num_bytes(group[3]);

        size_t write_pos = 0;
        const auto flag = static_cast<uint8_t>(((n1_minus1 - 1) << 6) | ((n2_minus1 - 1) << 4) |
                                               ((n3_minus1 - 1) << 2) | (n4_minus1 - 1));
        scratch[write_pos++] = flag;
        write_int_le(scratch, write_pos, group[0], n1_minus1);
        write_int_le(scratch, write_pos, group[1], n2_minus1);
        write_int_le(scratch, write_pos, group[2], n3_minus1);
        write_int_le(scratch, write_pos, group[3], n4_minus1);

        auto written = data_output.write_bytes(std::span<const uint8_t>(scratch.data(), write_pos));
        if (!written) {
            return written;
        }
        read_pos += 4;
    }

    // tail vints
    while (read_pos < limit) {
        auto narrowed = to_int(values[read_pos]);
        if (!narrowed) {
            return std::unexpected(std::move(narrowed.error()));
        }
        auto written = data_output.write_vint(narrowed.value());
        if (!written) {
            return written;
        }
        ++read_pos;
    }
    return {};
}

VoidResult GroupVIntUtil::write_group_vints_i32(DataOutput &data_output,
                                                std::span<uint8_t> scratch,
                                                std::span<const int32_t> values,
                                                size_t limit) {
    if (limit > values.size()) {
        return err(LuceneError::illegal_argument("values is too small for the requested limit"));
    }
    if (scratch.size() < MAX_LENGTH_PER_GROUP) {
        return err(LuceneError::illegal_argument("scratch must provide MAX_LENGTH_PER_GROUP bytes"));
    }

    size_t read_pos = 0;
    while (limit - read_pos >= 4) {
        const size_t n1_minus1 = num_bytes(values[read_pos]);
        const size_t n2_minus1 = num_bytes(values[read_pos + 1]);
        const size_t n3_minus1 = num_bytes(values[read_pos + 2]);
        const size_t n4_minus1 = num_bytes(values[read_pos + 3]);

        size_t write_pos = 0;
        const auto flag = static_cast<uint8_t>(((n1_minus1 - 1) << 6) | ((n2_minus1 - 1) << 4) |
                                               ((n3_minus1 - 1) << 2) | (n4_minus1 - 1));
        scratch[write_pos++] = flag;
        write_int_le(scratch, write_pos, values[read_pos], n1_minus1);
        write_int_le(scratch, write_pos, values[read_pos + 1], n2_minus1);
        write_int_le(scratch, write_pos, values[read_pos + 2], n3_minus1);
        write_int_le(scratch, write_pos, values[read_pos + 3], n4_minus1);

        auto written = data_output.write_bytes(std::span<const uint8_t>(scratch.data(), write_pos));
        if (!written) {
            return written;
        }
        read_pos += 4;
    }

    // tail vints
    while (read_pos < limit) {
        auto written = data_output.write_vint(values[read_pos]);
        if (!written) {
            return written;
        }
        ++read_pos;
    }
    return {};
}

} // namespace lucene
