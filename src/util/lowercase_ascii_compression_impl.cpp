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

module lucene_core:lowercase_ascii_compression.impl;

import :lowercase_ascii_compression;
import :types;
import :error;
import :debug;
import :data_input;
import :data_output;

import std;

namespace lucene {

namespace {

/// `(b + 1) & 0xA0 == 0x20` selects `[0x1F, 0x3F)` and `[0x5F, 0x7F)`.
constexpr bool is_compressible(int32_t b) noexcept {
    return ((b + 1) & 0xA0) == 0x20;
}

} // namespace

Result<bool> LowercaseAsciiCompression::compress(std::span<const uint8_t> input, size_t len, std::span<uint8_t> tmp,
                                                 DataOutput &out) {
    if (len < 8) {
        return false;
    }
    if (input.size() < len || tmp.size() < len) {
        return err(LuceneError::illegal_argument("input and tmp must hold at least len bytes"));
    }

    // 1. Count exceptions and fail compression if there are too many of them.
    const size_t max_exceptions = len >> 5;
    size_t previous_exception_index = 0;
    size_t num_exceptions = 0;
    for (size_t i = 0; i < len; ++i) {
        if (!is_compressible(static_cast<int32_t>(input[i]))) {
            while (i - previous_exception_index > 0xFF) {
                ++num_exceptions;
                previous_exception_index += 0xFF;
            }
            ++num_exceptions;
            if (num_exceptions > max_exceptions) {
                return false;
            }
            previous_exception_index = i;
        }
    }

    // 2. Move to 6-bit space.
    const size_t compressed_len = len - (len >> 2);
    debug_check(compressed_len < len, "the 6-bit packing must be smaller than the input");
    for (size_t i = 0; i < len; ++i) {
        const int32_t b = static_cast<int32_t>(input[i]) + 1;
        tmp[i] = static_cast<uint8_t>((b & 0x1F) | ((b & 0x40) >> 1));
    }

    // 3. Pack the exception bits into `tmp[0..compressed_len]`.
    {
        size_t o = 0;
        for (size_t i = compressed_len; i < len; ++i) {
            tmp[o] |= static_cast<uint8_t>((tmp[i] & 0x30) << 2); // bits 4-5
            ++o;
        }
        for (size_t i = compressed_len; i < len; ++i) {
            tmp[o] |= static_cast<uint8_t>((tmp[i] & 0x0C) << 4); // bits 2-3
            ++o;
        }
        for (size_t i = compressed_len; i < len; ++i) {
            tmp[o] |= static_cast<uint8_t>((tmp[i] & 0x03) << 6); // bits 0-1
            ++o;
        }
        debug_check(o <= compressed_len, "the packed bitmap must fit the compressed length");
    }
    if (auto result = out.write_bytes(tmp, 0, compressed_len); !result.has_value()) {
        return err(result.error());
    }

    // 4. Write the exception deltas.
    if (auto result = out.write_vint(static_cast<int32_t>(num_exceptions)); !result.has_value()) {
        return err(result.error());
    }
    if (num_exceptions > 0) {
        previous_exception_index = 0;
        size_t num_exceptions2 = 0;
        for (size_t i = 0; i < len; ++i) {
            if (!is_compressible(static_cast<int32_t>(input[i]))) {
                while (i - previous_exception_index > 0xFF) {
                    // Deltas are stored as bytes, so artificially split deltas
                    // greater than the maximum unsigned byte value.
                    if (auto result = out.write_byte(0xFF); !result.has_value()) {
                        return err(result.error());
                    }
                    previous_exception_index += 0xFF;
                    if (auto result = out.write_byte(input[previous_exception_index]); !result.has_value()) {
                        return err(result.error());
                    }
                    ++num_exceptions2;
                }
                if (auto result = out.write_byte(static_cast<uint8_t>(i - previous_exception_index));
                    !result.has_value()) {
                    return err(result.error());
                }
                previous_exception_index = i;
                if (auto result = out.write_byte(input[i]); !result.has_value()) {
                    return err(result.error());
                }
                ++num_exceptions2;
            }
        }
        if (num_exceptions != num_exceptions2) {
            return err(LuceneError::illegal_state(std::to_string(num_exceptions) + " <> " +
                                                  std::to_string(num_exceptions2) + " (" +
                                                  std::to_string(len) + " bytes)"));
        }
    }

    return true;
}

VoidResult LowercaseAsciiCompression::decompress(DataInput &input, std::span<uint8_t> out, size_t len) {
    if (out.size() < len) {
        return err(LuceneError::illegal_argument("output must hold at least len bytes"));
    }
    const size_t saved = len >> 2;
    const size_t compressed_len = len - saved;

    // 1. Copy the packed bytes.
    if (auto result = input.read_bytes(out, 0, compressed_len); !result.has_value()) {
        return result;
    }

    // 2. Restore the leading 2 bits into whole bytes.
    for (size_t i = 0; i < saved; ++i) {
        const uint8_t a = static_cast<uint8_t>((out[i] & 0xC0) >> 2);
        const uint8_t b = static_cast<uint8_t>((out[saved + i] & 0xC0) >> 4);
        const uint8_t c = static_cast<uint8_t>((out[(saved << 1) + i] & 0xC0) >> 6);
        out[compressed_len + i] = static_cast<uint8_t>(a | b | c);
    }

    // 3. Move back to the original range.
    for (size_t i = 0; i < len; ++i) {
        out[i] = static_cast<uint8_t>(((out[i] & 0x1F) | 0x20 | ((out[i] & 0x20) << 1)) - 1);
    }

    // 4. Restore the exceptions.
    auto num_exceptions = input.read_vint();
    if (!num_exceptions.has_value()) {
        return err(num_exceptions.error());
    }
    size_t i = 0;
    for (int32_t j = 0; j < num_exceptions.value(); ++j) {
        auto delta = input.read_byte();
        if (!delta.has_value()) {
            return err(delta.error());
        }
        i += delta.value();
        auto value = input.read_byte();
        if (!value.has_value()) {
            return err(value.error());
        }
        if (i >= len) {
            return err(LuceneError::illegal_state("exception index out of bounds"));
        }
        out[i] = value.value();
    }

    return {};
}

} // namespace lucene
