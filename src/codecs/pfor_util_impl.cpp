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

module lucene_core:pfor_util.impl;

import :pfor_util;

import :types;
import std;

import :error;
import :debug;
import :data_input;
import :data_output;
import :for_util;
import :long_heap;
import :packed_ints;
import :posting_decoding_util;

namespace lucene {

namespace {

[[nodiscard]] inline uint32_t left_shift_u32(uint32_t value, int32_t shift) noexcept {
    return shift >= 32 ? 0u : (value << static_cast<uint32_t>(shift));
}

} // namespace

bool PForUtil::all_equal(std::span<const int32_t> arr) {
    debug_check(arr.size() >= ForUtil::BLOCK_SIZE, "all_equal requires 128 values");
    const int32_t expected = arr[0];
    for (size_t i = 1; i < ForUtil::BLOCK_SIZE; ++i) {
        if (arr[i] != expected) {
            return false;
        }
    }
    return true;
}

VoidResult PForUtil::encode(std::span<int32_t> ints, DataOutput &out) {
    if (ints.size() < ForUtil::BLOCK_SIZE) {
        return err(LuceneError::illegal_argument("encode requires 128 values"));
    }
    if (!top_) {
        auto created = LongHeap::create(MAX_EXCEPTIONS + 1);
        if (!created) {
            return std::unexpected(std::move(created.error()));
        }
        top_.emplace(std::move(created.value()));
    }
    LongHeap &top = *top_;
    // Also reset after an earlier encode returned an error.
    top.clear();
    for (size_t i = 0; i <= MAX_EXCEPTIONS; ++i) {
        auto pushed = top.push(ints[i]);
        if (!pushed) {
            return std::unexpected(std::move(pushed.error()));
        }
    }

    int64_t top_value = top.top();
    for (size_t i = MAX_EXCEPTIONS + 1; i < ForUtil::BLOCK_SIZE; ++i) {
        if (static_cast<int64_t>(ints[i]) > top_value) {
            top_value = top.update_top(ints[i]);
        }
    }

    int64_t max = 0;
    for (size_t i = 1; i <= top.size(); ++i) {
        max = std::max(max, top.get(i));
    }

    auto max_bits = PackedInts::bits_required(max);
    if (!max_bits) {
        return std::unexpected(std::move(max_bits.error()));
    }
    auto top_bits = PackedInts::bits_required(top_value);
    if (!top_bits) {
        return std::unexpected(std::move(top_bits.error()));
    }
    const int32_t max_bits_required = max_bits.value();
    // The patch is stored on a byte, so patching cannot save more than 8 bits.
    const int32_t patched_bits_required = std::max(top_bits.value(), max_bits_required - 8);

    size_t num_exceptions = 0;
    const int64_t max_unpatched_value = (int64_t{1} << patched_bits_required) - 1;
    for (size_t i = 2; i <= top.size(); ++i) {
        if (top.get(i) > max_unpatched_value) {
            ++num_exceptions;
        }
    }

    std::array<uint8_t, MAX_EXCEPTIONS * 2> exceptions{};
    if (num_exceptions > 0) {
        size_t exception_count = 0;
        for (size_t i = 0; i < ForUtil::BLOCK_SIZE; ++i) {
            if (static_cast<int64_t>(ints[i]) > max_unpatched_value) {
                exceptions[exception_count * 2] = static_cast<uint8_t>(i);
                exceptions[exception_count * 2 + 1] = static_cast<uint8_t>(
                        static_cast<uint64_t>(static_cast<int64_t>(ints[i])) >> patched_bits_required);
                ints[i] = static_cast<int32_t>(static_cast<int64_t>(ints[i]) & max_unpatched_value);
                ++exception_count;
            }
        }
        debug_check(exception_count == num_exceptions, "exception count mismatch");
    }

    if (all_equal(ints) && max_bits_required <= 8) {
        for (size_t i = 0; i < num_exceptions; ++i) {
            exceptions[2 * i + 1] = static_cast<uint8_t>(
                    left_shift_u32(exceptions[2 * i + 1], patched_bits_required));
        }
        auto token = out.write_byte(static_cast<uint8_t>(num_exceptions << 5));
        if (!token) {
            return token;
        }
        auto value = out.write_vint(ints[0]);
        if (!value) {
            return value;
        }
    } else {
        const auto token = static_cast<uint8_t>((num_exceptions << 5) |
                                               static_cast<size_t>(patched_bits_required));
        auto written = out.write_byte(token);
        if (!written) {
            return written;
        }
        auto encoded = for_util_.encode(ints, patched_bits_required, out);
        if (!encoded) {
            return encoded;
        }
    }

    return out.write_bytes(std::span<const uint8_t>(exceptions.data(), num_exceptions * 2));
}

VoidResult PForUtil::decode(PostingDecodingUtil &pdu, std::span<int32_t> ints) {
    if (ints.size() < ForUtil::BLOCK_SIZE) {
        return err(LuceneError::illegal_argument("decode requires 128 values"));
    }
    auto token = pdu.input().read_byte();
    if (!token) {
        return std::unexpected(std::move(token.error()));
    }
    const int32_t bits_per_value = token.value() & 0x1F;

    if (bits_per_value == 0) {
        auto value = pdu.input().read_vint();
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        std::fill_n(ints.begin(), ForUtil::BLOCK_SIZE, value.value());
    } else {
        auto decoded = for_util_.decode(bits_per_value, pdu, ints);
        if (!decoded) {
            return decoded;
        }
    }

    const size_t num_exceptions = token.value() >> 5;
    for (size_t i = 0; i < num_exceptions; ++i) {
        auto index = pdu.input().read_byte();
        if (!index) {
            return std::unexpected(std::move(index.error()));
        }
        auto patch = pdu.input().read_byte();
        if (!patch) {
            return std::unexpected(std::move(patch.error()));
        }
        ints[index.value()] = static_cast<int32_t>(
                static_cast<uint32_t>(ints[index.value()]) |
                left_shift_u32(patch.value(), bits_per_value));
    }
    return {};
}

VoidResult PForUtil::skip(DataInput &input) {
    auto token = input.read_byte();
    if (!token) {
        return std::unexpected(std::move(token.error()));
    }
    const int32_t bits_per_value = token.value() & 0x1F;
    const int64_t num_exceptions = token.value() >> 5;

    if (bits_per_value == 0) {
        auto value = input.read_vlong();
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        return input.skip_bytes(num_exceptions << 1);
    }
    return input.skip_bytes(ForUtil::num_bytes(bits_per_value) + (num_exceptions << 1));
}

} // namespace lucene
