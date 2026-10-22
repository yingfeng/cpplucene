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

module lucene_core:packed_ints.impl;

import :packed_ints;
import :types;
import :error;
import :core_helper;

import std;

namespace lucene {

Result<int32_t> PackedInts::bits_required(int64_t max_value) {
    if (max_value < 0) {
        return err(LuceneError::illegal_argument("max_value must be non-negative (got " +
                                                 std::to_string(max_value) + ")"));
    }
    return unsigned_bits_required(max_value);
}

VoidResult PackedInts::check_version(int32_t version) {
    if (version < VERSION_START) {
        return err(LuceneError::illegal_argument("Version is too old, should be at least " +
                                                 std::to_string(VERSION_START) + " (got " + std::to_string(version) +
                                                 ")"));
    }
    if (version > VERSION_CURRENT) {
        return err(LuceneError::illegal_argument("Version is too new, should be at most " +
                                                 std::to_string(VERSION_CURRENT) + " (got " +
                                                 std::to_string(version) + ")"));
    }
    return {};
}

Result<size_t> PackedInts::num_blocks(size_t size, size_t block_size) {
    if (block_size < 1) {
        return err(LuceneError::illegal_argument("block_size must be greater than 0"));
    }
    return (size + block_size - 1) / block_size;
}

size_t packed_byte_count(int32_t version, int64_t value_count, int32_t bits_per_value) {
    // `Format.PACKED.byteCount`: the bits are contiguous, so this is independent
    // of the version.
    (void)version;
    if (value_count == 0 || bits_per_value == 0) {
        return 0;
    }
    const uint64_t bits = static_cast<uint64_t>(value_count) * static_cast<uint64_t>(bits_per_value);
    return static_cast<size_t>((bits + 7) / 8);
}

Result<size_t> packed_single_block_byte_count(int32_t version, int64_t value_count, int32_t bits_per_value) {
    (void)version;
    if (bits_per_value != 1 && bits_per_value != 2 && bits_per_value != 4 && bits_per_value != 8 &&
        bits_per_value != 16 && bits_per_value != 32 && bits_per_value != 64) {
        return err(LuceneError::illegal_argument("PACKED_SINGLE_BLOCK does not support bitsPerValue=" +
                                                 std::to_string(bits_per_value)));
    }
    if (value_count == 0) {
        return size_t{0};
    }
    const int32_t values_per_block = 64 / bits_per_value;
    const uint64_t blocks =
        (static_cast<uint64_t>(value_count) + static_cast<uint64_t>(values_per_block) - 1) /
        static_cast<uint64_t>(values_per_block);
    return static_cast<size_t>(blocks * 8);
}

FormatAndBits fastest_format_and_bits(int32_t value_count, int32_t bits_per_value,
                                      float acceptable_overhead_ratio) {
    (void)value_count;
    acceptable_overhead_ratio = std::max(PackedInts::COMPACT, acceptable_overhead_ratio);
    acceptable_overhead_ratio = std::min(PackedInts::FASTEST, acceptable_overhead_ratio);
    const float acceptable_overhead_per_value = acceptable_overhead_ratio * static_cast<float>(bits_per_value);
    const int32_t max_bits_per_value = bits_per_value + static_cast<int32_t>(acceptable_overhead_per_value);

    int32_t actual_bits_per_value = bits_per_value;
    if (bits_per_value <= 8 && max_bits_per_value >= 8) {
        actual_bits_per_value = 8;
    } else if (bits_per_value <= 16 && max_bits_per_value >= 16) {
        actual_bits_per_value = 16;
    } else if (bits_per_value <= 32 && max_bits_per_value >= 32) {
        actual_bits_per_value = 32;
    } else if (bits_per_value <= 64 && max_bits_per_value >= 64) {
        actual_bits_per_value = 64;
    }
    return FormatAndBits{PackedFormat::kPacked, actual_bits_per_value};
}

} // namespace lucene
