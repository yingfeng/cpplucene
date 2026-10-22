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

module lucene_core:codec_util.impl;

import :codec_util;
import :types;
import :error;
import :debug;
import :data_input;
import :data_output;
import :index_input;
import :index_output;

import std;

namespace lucene {

VoidResult CodecUtil::write_be_int(DataOutput &out, int32_t value) {
    const std::array<uint8_t, 4> bytes{
        static_cast<uint8_t>((value >> 24) & 0xFF),
        static_cast<uint8_t>((value >> 16) & 0xFF),
        static_cast<uint8_t>((value >> 8) & 0xFF),
        static_cast<uint8_t>(value & 0xFF),
    };
    return out.write_bytes(bytes);
}

VoidResult CodecUtil::write_be_long(DataOutput &out, int64_t value) {
    std::array<uint8_t, 8> bytes{};
    for (size_t i = 0; i < 8; ++i) {
        bytes[i] = static_cast<uint8_t>((static_cast<uint64_t>(value) >> (56 - 8 * i)) & 0xFF);
    }
    return out.write_bytes(bytes);
}

Result<int32_t> CodecUtil::read_be_int(DataInput &input) {
    std::array<uint8_t, 4> bytes{};
    if (auto result = input.read_bytes(bytes); !result.has_value()) {
        return err(result.error());
    }
    return (static_cast<int32_t>(bytes[0]) << 24) | (static_cast<int32_t>(bytes[1]) << 16) |
           (static_cast<int32_t>(bytes[2]) << 8) | static_cast<int32_t>(bytes[3]);
}

Result<int64_t> CodecUtil::read_be_long(DataInput &input) {
    std::array<uint8_t, 8> bytes{};
    if (auto result = input.read_bytes(bytes); !result.has_value()) {
        return err(result.error());
    }
    uint64_t value = 0;
    for (const uint8_t byte : bytes) {
        value = (value << 8) | byte;
    }
    return static_cast<int64_t>(value);
}

VoidResult CodecUtil::write_header(DataOutput &out, std::string_view codec, int32_t version) {
    if (codec.size() >= 128) {
        return err(LuceneError::illegal_argument(
            "codec must be simple ASCII, less than 128 characters in length [got " + std::string(codec) + "]"));
    }
    for (const char character : codec) {
        if (static_cast<unsigned char>(character) >= 0x80) {
            return err(LuceneError::illegal_argument(
                "codec must be simple ASCII, less than 128 characters in length [got " + std::string(codec) + "]"));
        }
    }
    if (auto result = write_be_int(out, CODEC_MAGIC); !result.has_value()) {
        return result;
    }
    if (auto result = out.write_string(codec); !result.has_value()) {
        return result;
    }
    return write_be_int(out, version);
}

Result<int32_t> CodecUtil::check_header(DataInput &input, std::string_view codec, int32_t min_version,
                                        int32_t max_version) {
    auto magic = read_be_int(input);
    if (!magic.has_value()) {
        return err(magic.error());
    }
    if (magic.value() != CODEC_MAGIC) {
        return err(LuceneError::corrupt_index("codec header mismatch: actual header=" +
                                              std::to_string(magic.value()) + " vs expected header=" +
                                              std::to_string(CODEC_MAGIC)));
    }
    return check_header_no_magic(input, codec, min_version, max_version);
}

Result<int32_t> CodecUtil::check_header_no_magic(DataInput &input, std::string_view codec, int32_t min_version,
                                                 int32_t max_version) {
    auto actual_codec = input.read_string();
    if (!actual_codec.has_value()) {
        return err(actual_codec.error());
    }
    if (actual_codec.value() != codec) {
        return err(LuceneError::corrupt_index("codec mismatch: actual codec=" + actual_codec.value() +
                                              " vs expected codec=" + std::string(codec)));
    }
    auto version = read_be_int(input);
    if (!version.has_value()) {
        return err(version.error());
    }
    if (version.value() < min_version) {
        return err(LuceneError(ErrorKind::kIndexFormatTooOld,
                               "Format version is too old: " + std::to_string(version.value()) + " (needs to be between " +
                                   std::to_string(min_version) + " and " + std::to_string(max_version) + ")"));
    }
    if (version.value() > max_version) {
        return err(LuceneError(ErrorKind::kIndexFormatTooNew,
                               "Format version is too new: " + std::to_string(version.value()) + " (needs to be between " +
                                   std::to_string(min_version) + " and " + std::to_string(max_version) + ")"));
    }
    return version;
}

size_t CodecUtil::header_length(std::string_view codec) {
    // magic (4) + vint length (1, the name is shorter than 128) + name + version (4)
    return 4 + 1 + codec.size() + 4;
}

VoidResult CodecUtil::write_crc(IndexOutput &out) {
    const uint64_t value = out.get_checksum();
    if ((value & 0xFFFF'FFFF'0000'0000ULL) != 0) {
        return err(LuceneError::illegal_state("Illegal CRC-32 checksum: " + std::to_string(value)));
    }
    return write_be_long(out, static_cast<int64_t>(value));
}

Result<int64_t> CodecUtil::read_crc(IndexInput &input) {
    auto value = read_be_long(input);
    if (!value.has_value()) {
        return err(value.error());
    }
    if ((static_cast<uint64_t>(value.value()) & 0xFFFF'FFFF'0000'0000ULL) != 0) {
        return err(LuceneError::corrupt_index("Illegal CRC-32 checksum: " + std::to_string(value.value()) + " (resource=" +
                                              std::string(input.to_string()) + ")"));
    }
    return value;
}

VoidResult CodecUtil::write_footer(IndexOutput &out) {
    if (auto result = write_be_int(out, FOOTER_MAGIC); !result.has_value()) {
        return result;
    }
    if (auto result = write_be_int(out, 0); !result.has_value()) {
        return result;
    }
    return write_crc(out);
}

VoidResult CodecUtil::validate_footer(DataInput &input) {
    auto magic = read_be_int(input);
    if (!magic.has_value()) {
        return err(magic.error());
    }
    if (magic.value() != FOOTER_MAGIC) {
        return err(LuceneError::corrupt_index("codec footer mismatch (file truncated?): actual footer=" +
                                              std::to_string(magic.value()) + " vs expected footer=" +
                                              std::to_string(FOOTER_MAGIC)));
    }
    auto algorithm = read_be_int(input);
    if (!algorithm.has_value()) {
        return err(algorithm.error());
    }
    if (algorithm.value() != 0) {
        return err(LuceneError::corrupt_index("unknown checksum algorithm: id=" + std::to_string(algorithm.value())));
    }
    return {};
}

Result<int64_t> CodecUtil::check_footer(DataInput &input) {
    if (auto result = validate_footer(input); !result.has_value()) {
        return err(result.error());
    }
    IndexInput *as_index_input = dynamic_cast<IndexInput *>(&input);
    if (as_index_input == nullptr) {
        // No checksum tracking available: report what the footer claims.
        std::array<uint8_t, 8> bytes{};
        if (auto result = input.read_bytes(bytes); !result.has_value()) {
            return err(result.error());
        }
        uint64_t value = 0;
        for (const uint8_t byte : bytes) {
            value = (value << 8) | byte;
        }
        return static_cast<int64_t>(value);
    }
    return read_crc(*as_index_input);
}

Result<int64_t> CodecUtil::check_footer(DataInput &input, int64_t actual_checksum) {
    auto expected = check_footer(input);
    if (!expected.has_value()) {
        return err(expected.error());
    }
    if (expected.value() != actual_checksum) {
        return err(LuceneError::corrupt_index("checksum failed (hardware problem?): expected=" +
                                              std::to_string(expected.value()) + " but got=" +
                                              std::to_string(actual_checksum)));
    }
    return actual_checksum;
}

Result<int64_t> CodecUtil::retrieve_checksum(IndexInput &input) {
    auto length = input.length();
    if (!length.has_value()) {
        return err(length.error());
    }
    if (length.value() < footer_length()) {
        return err(LuceneError::corrupt_index("misplaced codec footer (file truncated?): length=" +
                                              std::to_string(length.value()) + " but footerLength==" +
                                              std::to_string(footer_length())));
    }
    if (auto result = input.seek(length.value() - footer_length()); !result.has_value()) {
        return err(result.error());
    }
    if (auto result = validate_footer(input); !result.has_value()) {
        return err(result.error());
    }
    return read_crc(input);
}

} // namespace lucene
