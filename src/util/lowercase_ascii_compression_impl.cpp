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
    // Classify sixteen bytes at a time: a block whose bytes all satisfy the
    // compressible test contributes no exceptions, and the reduction over a block
    // is the only work it needs.  Any block with a candidate falls back to the
    // indexed loop, so the count and the failure point stay exactly as before.
    size_t block_start = 0;
    while (block_start < len) {
        const size_t block_end = block_start + std::min<size_t>(16, len - block_start);
        bool clean_block = block_end - block_start == 16;
        if (clean_block) {
            uint8_t bad = 0;
            for (size_t i = block_start; i < block_end; ++i) {
                bad |= static_cast<uint8_t>(((static_cast<uint32_t>(input[i]) + 1u) & 0xA0u) != 0x20u);
            }
            clean_block = bad == 0;
        }
        if (!clean_block) {
            for (size_t i = block_start; i < block_end; ++i) {
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
        }
        block_start = block_end;
    }

    // 2. Move to 6-bit space.
    const size_t compressed_len = len - (len >> 2);
    debug_check(compressed_len < len, "the 6-bit packing must be smaller than the input");
    for (size_t i = 0; i < len; ++i) {
        const int32_t b = static_cast<int32_t>(input[i]) + 1;
        tmp[i] = static_cast<uint8_t>((b & 0x1F) | ((b & 0x40) >> 1));
    }

    // 3. Pack the exception bits into `tmp[0..compressed_len]`.
    //
    // The three passes write disjoint quarters of the packed area, so they are
    // expressed as three non-overlapping slices instead of a running cursor.
    {
        const size_t saved = len >> 2;
        std::span<uint8_t> packed = tmp.first(compressed_len);
        const std::span<const uint8_t> tail = tmp.subspan(compressed_len, len - compressed_len);
        const std::span<uint8_t> first = packed.first(saved);
        const std::span<uint8_t> second = packed.subspan(saved, saved);
        const std::span<uint8_t> third = packed.subspan(2 * saved, saved);
        for (size_t i = 0; i < saved; ++i) {
            first[i] |= static_cast<uint8_t>((tail[i] & 0x30) << 2); // bits 4-5
        }
        for (size_t i = 0; i < saved; ++i) {
            second[i] |= static_cast<uint8_t>((tail[i] & 0x0C) << 4); // bits 2-3
        }
        for (size_t i = 0; i < saved; ++i) {
            third[i] |= static_cast<uint8_t>((tail[i] & 0x03) << 6); // bits 0-1
        }
        debug_check(saved * 3 <= compressed_len, "the packed bitmap must fit the compressed length");
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
