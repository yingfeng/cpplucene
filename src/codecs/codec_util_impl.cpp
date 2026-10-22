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
import :buffered_checksum_index_input;
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

VoidResult CodecUtil::write_index_header(DataOutput &out, std::string_view codec, int32_t version,
                                         std::span<const uint8_t> id, std::string_view suffix) {
    if (id.size() != ID_LENGTH) {
        return err(LuceneError::illegal_argument("the segment id must be " + std::to_string(ID_LENGTH) +
                                                 " bytes long, got " + std::to_string(id.size())));
    }
    if (auto written = write_header(out, codec, version); !written.has_value()) {
        return written;
    }
    if (auto written = out.write_bytes(id); !written.has_value()) {
        return written;
    }
    return out.write_string(suffix);
}

Result<int32_t> CodecUtil::check_index_header(DataInput &input, std::string_view codec, int32_t min_version,
                                             int32_t max_version, std::span<const uint8_t> id,
                                             std::string_view suffix) {
    auto version = check_header(input, codec, min_version, max_version);
    if (!version.has_value()) {
        return err(version.error());
    }

    std::vector<uint8_t> actual_id(ID_LENGTH, 0);
    if (auto read = input.read_bytes(actual_id); !read.has_value()) {
        return err(read.error());
    }
    if (id.size() == ID_LENGTH && !std::equal(actual_id.begin(), actual_id.end(), id.begin())) {
        return err(LuceneError::corrupt_index("segment id mismatch (resource=" + std::string(input.to_string()) + ")"));
    }

    auto actual_suffix = input.read_string();
    if (!actual_suffix.has_value()) {
        return err(actual_suffix.error());
    }
    if (actual_suffix.value() != suffix) {
        return err(LuceneError::corrupt_index("codec suffix mismatch: actual='" + actual_suffix.value() +
                                              "' vs expected='" + std::string(suffix) + "' (resource=" +
                                              std::string(input.to_string()) + ")"));
    }
    return version;
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

Result<int64_t> CodecUtil::retrieve_checksum_with_expected(IndexInput &input, size_t expected_length) {
    if (expected_length < footer_length()) {
        return err(LuceneError::illegal_argument("expectedLength cannot be less than the footer length"));
    }
    auto length = input.length();
    if (!length) {
        return err(length.error());
    }
    if (length.value() < expected_length) {
        return err(LuceneError::corrupt_index(std::format(
                "truncated file: length={} but expected_length={} (resource={})", length.value(), expected_length,
                input.to_string())));
    }
    if (length.value() > expected_length) {
        return err(LuceneError::corrupt_index(std::format(
                "file too long: length={} but expected_length={} (resource={})", length.value(), expected_length,
                input.to_string())));
    }
    return retrieve_checksum(input);
}

Result<int64_t> CodecUtil::checksum_entire_file(IndexInput &input) {
    auto clone = input.clone();
    if (!clone) {
        return err(clone.error());
    }
    if (auto seek = clone.value()->seek(0); !seek) {
        return err(seek.error());
    }
    BufferedChecksumIndexInput checksum_in{std::move(clone.value())};
    auto length = checksum_in.length();
    if (!length) {
        return err(length.error());
    }
    if (length.value() < footer_length()) {
        return err(LuceneError::corrupt_index(std::format(
                "misplaced codec footer (file truncated?): length={} but footerLength=={} (resource={})",
                length.value(), footer_length(), input.to_string())));
    }
    if (auto seek = checksum_in.seek(length.value() - footer_length()); !seek) {
        return err(seek.error());
    }
    // The writer samples its checksum after it has written FOOTER_MAGIC and the
    // algorithm id (`write_footer` writes them before `write_crc`), so the
    // reader has to consume exactly those two ints before comparing.
    if (auto validated = validate_footer(checksum_in); !validated) {
        return err(validated.error());
    }
    const int64_t computed = checksum_in.get_checksum();
    auto stored = read_crc(checksum_in);
    if (!stored) {
        return err(stored.error());
    }
    if (stored.value() != computed) {
        return err(LuceneError::corrupt_index(std::format(
                "checksum failed (hardware problem?): stored={} computed={} (resource={})", stored.value(), computed,
                input.to_string())));
    }
    return stored.value();
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


VoidResult CodecUtil::verify_and_copy_index_header(IndexInput &input, DataOutput &output,
                                                   std::span<const uint8_t> expected_id) {
    auto length = input.length();
    if (!length.has_value()) {
        return err(length.error());
    }
    if (length.value() < footer_length() + header_length("")) {
        return err(LuceneError::corrupt_index("compound sub-files must have a valid codec header and footer: file is "
                                              "too small (" +
                                              std::to_string(length.value()) + " bytes)"));
    }

    auto actual_header = read_be_int(input);
    if (!actual_header.has_value()) {
        return err(actual_header.error());
    }
    if (actual_header.value() != CODEC_MAGIC) {
        return err(LuceneError::corrupt_index("compound sub-files must have a valid codec header and footer: codec "
                                              "header mismatch: actual header=" +
                                              std::to_string(actual_header.value()) +
                                              " vs expected header=" + std::to_string(CODEC_MAGIC)));
    }

    auto codec = input.read_string();
    if (!codec.has_value()) {
        return err(codec.error());
    }
    auto version = read_be_int(input);
    if (!version.has_value()) {
        return err(version.error());
    }

    std::vector<uint8_t> actual_id(ID_LENGTH);
    if (auto read = input.read_bytes(actual_id); !read.has_value()) {
        return err(read.error());
    }
    if (!std::ranges::equal(actual_id, expected_id)) {
        return err(LuceneError::corrupt_index("compound sub-files must have a valid codec header and footer: segment "
                                              "id mismatch (resource=" +
                                              std::string(input.to_string()) + ")"));
    }

    auto suffix_length = input.read_byte();
    if (!suffix_length.has_value()) {
        return err(suffix_length.error());
    }
    std::vector<uint8_t> suffix(suffix_length.value());
    if (!suffix.empty()) {
        if (auto read = input.read_bytes(suffix); !read.has_value()) {
            return err(read.error());
        }
    }

    // Write the header, without changing anything.
    if (auto write = write_be_int(output, CODEC_MAGIC); !write.has_value()) {
        return write;
    }
    if (auto write = output.write_string(codec.value()); !write.has_value()) {
        return write;
    }
    if (auto write = write_be_int(output, version.value()); !write.has_value()) {
        return write;
    }
    if (auto write = output.write_bytes(expected_id); !write.has_value()) {
        return write;
    }
    if (auto write = output.write_byte(suffix_length.value()); !write.has_value()) {
        return write;
    }
    if (!suffix.empty()) {
        if (auto write = output.write_bytes(suffix); !write.has_value()) {
            return write;
        }
    }
    return {};
}

} // namespace lucene
