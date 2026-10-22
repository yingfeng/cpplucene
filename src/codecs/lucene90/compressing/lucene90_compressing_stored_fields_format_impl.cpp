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

module lucene_core:lucene90_compressing_stored_fields_format.impl;

import :lucene90_compressing_stored_fields_format;

import :bit_util;
import :compression;
import :data_input;
import :data_output;
import :direct_monotonic_writer;
import :error;
import :lucene90_compressing_stored_fields_reader;
import :lucene90_compressing_stored_fields_writer;
import :stored_fields_format;

import std;

namespace lucene {

namespace {

constexpr uint32_t NEGATIVE_ZERO_FLOAT = 0x80000000U;
constexpr uint64_t NEGATIVE_ZERO_DOUBLE = 0x8000000000000000ULL;

constexpr int64_t SECOND = 1000;
constexpr int64_t HOUR = 60 * 60 * SECOND;
constexpr int64_t DAY = 24 * HOUR;

constexpr int32_t SECOND_ENCODING = 0x40;
constexpr int32_t HOUR_ENCODING = 0x80;
constexpr int32_t DAY_ENCODING = 0xC0;

} // namespace

VoidResult write_zfloat(DataOutput &out, float f) {
    const int32_t int_val = static_cast<int32_t>(f);
    const uint32_t float_bits = BitUtil::float_to_int_bits(f);
    if (f == static_cast<float>(int_val) && int_val >= -1 && int_val <= 0x7D && float_bits != NEGATIVE_ZERO_FLOAT) {
        // A small integer in [-1..125]: a single byte.
        return out.write_byte(static_cast<uint8_t>(0x80 | (1 + int_val)));
    }
    if ((float_bits >> 31) == 0) {
        // Another positive float: four bytes, the first of which is the high byte.
        if (auto written = out.write_byte(static_cast<uint8_t>(float_bits >> 24)); !written.has_value()) {
            return written;
        }
        if (auto written = out.write_short(static_cast<int16_t>(float_bits >> 8)); !written.has_value()) {
            return written;
        }
        return out.write_byte(static_cast<uint8_t>(float_bits));
    }
    // A negative float or a special value: five bytes.
    if (auto written = out.write_byte(0xFF); !written.has_value()) {
        return written;
    }
    return out.write_int(static_cast<int32_t>(float_bits));
}

Result<float> read_zfloat(DataInput &in) {
    auto b = in.read_byte();
    if (!b.has_value()) {
        return std::unexpected(std::move(b.error()));
    }
    const int32_t header = b.value();
    if (header == 0xFF) {
        auto bits = in.read_int();
        if (!bits.has_value()) {
            return std::unexpected(std::move(bits.error()));
        }
        return std::bit_cast<float>(static_cast<uint32_t>(bits.value()));
    }
    if ((header & 0x80) != 0) {
        return static_cast<float>((header & 0x7F) - 1);
    }
    const uint32_t high = static_cast<uint32_t>(header) << 24;
    auto mid = in.read_short();
    if (!mid.has_value()) {
        return std::unexpected(std::move(mid.error()));
    }
    auto low = in.read_byte();
    if (!low.has_value()) {
        return std::unexpected(std::move(low.error()));
    }
    const uint32_t bits = high | (static_cast<uint32_t>(static_cast<uint16_t>(mid.value())) << 8) | low.value();
    return std::bit_cast<float>(bits);
}

VoidResult write_zdouble(DataOutput &out, double d) {
    const int32_t int_val = static_cast<int32_t>(d);
    const uint64_t double_bits = BitUtil::double_to_long_bits(d);
    if (d == static_cast<double>(int_val) && int_val >= -1 && int_val <= 0x7C &&
        double_bits != NEGATIVE_ZERO_DOUBLE) {
        // A small integer in [-1..124]: a single byte.
        return out.write_byte(static_cast<uint8_t>(0x80 | (1 + int_val)));
    }
    if (d == static_cast<double>(static_cast<float>(d))) {
        // The value is exactly representable as a float: five bytes.
        if (auto written = out.write_byte(0xFE); !written.has_value()) {
            return written;
        }
        return out.write_int(static_cast<int32_t>(BitUtil::float_to_int_bits(static_cast<float>(d))));
    }
    if ((double_bits >> 63) == 0) {
        // Another positive double: eight bytes.
        if (auto written = out.write_byte(static_cast<uint8_t>(double_bits >> 56)); !written.has_value()) {
            return written;
        }
        if (auto written = out.write_int(static_cast<int32_t>(static_cast<uint32_t>(double_bits >> 24)));
            !written.has_value()) {
            return written;
        }
        if (auto written = out.write_short(static_cast<int16_t>(double_bits >> 8)); !written.has_value()) {
            return written;
        }
        return out.write_byte(static_cast<uint8_t>(double_bits));
    }
    // Another negative double: nine bytes.
    if (auto written = out.write_byte(0xFF); !written.has_value()) {
        return written;
    }
    return out.write_long(static_cast<int64_t>(double_bits));
}

Result<double> read_zdouble(DataInput &in) {
    auto b = in.read_byte();
    if (!b.has_value()) {
        return std::unexpected(std::move(b.error()));
    }
    const int32_t header = b.value();
    if (header == 0xFF) {
        auto bits = in.read_long();
        if (!bits.has_value()) {
            return std::unexpected(std::move(bits.error()));
        }
        return std::bit_cast<double>(static_cast<uint64_t>(bits.value()));
    }
    if (header == 0xFE) {
        auto bits = in.read_int();
        if (!bits.has_value()) {
            return std::unexpected(std::move(bits.error()));
        }
        return static_cast<double>(std::bit_cast<float>(static_cast<uint32_t>(bits.value())));
    }
    if ((header & 0x80) != 0) {
        return static_cast<double>((header & 0x7F) - 1);
    }
    const uint64_t high = static_cast<uint64_t>(header) << 56;
    auto mid1 = in.read_int();
    if (!mid1.has_value()) {
        return std::unexpected(std::move(mid1.error()));
    }
    auto mid2 = in.read_short();
    if (!mid2.has_value()) {
        return std::unexpected(std::move(mid2.error()));
    }
    auto low = in.read_byte();
    if (!low.has_value()) {
        return std::unexpected(std::move(low.error()));
    }
    const uint64_t bits = high | (static_cast<uint64_t>(static_cast<uint32_t>(mid1.value())) << 24) |
                          (static_cast<uint64_t>(static_cast<uint16_t>(mid2.value())) << 8) | low.value();
    return std::bit_cast<double>(bits);
}

VoidResult write_tlong(DataOutput &out, int64_t value) {
    int32_t header = 0;
    int64_t scaled = value;
    if (value % SECOND != 0) {
        header = 0;
    } else if (value % DAY == 0) {
        // A timestamp with day precision.
        header = DAY_ENCODING;
        scaled = value / DAY;
    } else if (value % HOUR == 0) {
        // A timestamp with hour precision, or day precision with a time zone.
        header = HOUR_ENCODING;
        scaled = value / HOUR;
    } else {
        header = SECOND_ENCODING;
        scaled = value / SECOND;
    }

    const auto zigzag = static_cast<uint64_t>(BitUtil::zig_zag_encode_i64(scaled));
    header |= static_cast<int32_t>(zigzag & 0x1F);
    const int64_t upper_bits = static_cast<int64_t>(zigzag >> 5);
    if (upper_bits != 0) {
        header |= 0x20;
    }

    if (auto written = out.write_byte(static_cast<uint8_t>(header)); !written.has_value()) {
        return written;
    }
    if (upper_bits != 0) {
        return out.write_vlong(upper_bits);
    }
    return {};
}

Result<int64_t> read_tlong(DataInput &in) {
    auto b = in.read_byte();
    if (!b.has_value()) {
        return std::unexpected(std::move(b.error()));
    }
    const int32_t header = b.value();

    int64_t bits = header & 0x1F;
    if ((header & 0x20) != 0) {
        auto upper = in.read_vlong();
        if (!upper.has_value()) {
            return std::unexpected(std::move(upper.error()));
        }
        bits |= upper.value() << 5;
    }

    int64_t value = BitUtil::zig_zag_decode_i64(static_cast<uint64_t>(bits));
    switch (header & DAY_ENCODING) {
        case SECOND_ENCODING:
            value = value * SECOND;
            break;
        case HOUR_ENCODING:
            value = value * HOUR;
            break;
        case DAY_ENCODING:
            value = value * DAY;
            break;
        default:
            break;
    }
    return value;
}

Result<Lucene90CompressingStoredFieldsFormat> Lucene90CompressingStoredFieldsFormat::create(
        std::string format_name, CompressionMode compression_mode, int32_t chunk_size, int32_t max_docs_per_chunk,
        int32_t block_shift) {
    if (chunk_size < 1) {
        return err(LuceneError::illegal_argument("chunk_size must be >= 1"));
    }
    if (max_docs_per_chunk < 1) {
        return err(LuceneError::illegal_argument("max_docs_per_chunk must be >= 1"));
    }
    if (block_shift < MIN_BLOCK_SHIFT || block_shift > MAX_BLOCK_SHIFT) {
        return err(LuceneError::illegal_argument("block_shift must be in " + std::to_string(MIN_BLOCK_SHIFT) + "-" +
                                                 std::to_string(MAX_BLOCK_SHIFT) + ", got " +
                                                 std::to_string(block_shift)));
    }
    return Lucene90CompressingStoredFieldsFormat(std::move(format_name), compression_mode, chunk_size,
                                                 max_docs_per_chunk, block_shift);
}

Result<std::unique_ptr<StoredFieldsReader>> Lucene90CompressingStoredFieldsFormat::fields_reader(
        const Directory &directory, const SegmentInfo &segment_info,
        std::shared_ptr<const FieldInfos> field_infos, const IOContext &context) const {
    return Lucene90CompressingStoredFieldsReader::create(directory, segment_info, field_infos, context,
                                                         format_name_, compression_mode_);
}

Result<std::unique_ptr<StoredFieldsWriter>> Lucene90CompressingStoredFieldsFormat::fields_writer(
        Directory &directory, SegmentInfo &segment_info, const IOContext &context) const {
    return Lucene90CompressingStoredFieldsWriter::create(directory, segment_info, context, format_name_,
                                                         compression_mode_, chunk_size_, max_docs_per_chunk_,
                                                         block_shift_);
}

} // namespace lucene
