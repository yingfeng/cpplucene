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

module lucene_core:direct_writer.impl;

import :direct_writer;
import :packed_ints;
import :types;
import :error;
import :debug;
import :bit_util;
import :data_output;

import std;

namespace lucene {

DirectWriter::DirectWriter(DataOutput &output, int64_t num_values, int32_t bits_per_value)
    : output_(&output), bits_per_value_(bits_per_value), num_values_(num_values) {
    // For every value the buffer holds 64 bits of value plus `bits_per_value`
    // bits of encoded output.
    const int32_t memory_budget_in_bits = 8 * PackedInts::DEFAULT_BUFFER_SIZE;
    int32_t buffer_size = memory_budget_in_bits / (64 + bits_per_value);
    debug_check(buffer_size > 0, "buffer size must be positive");
    // Round up to the next multiple of 64 so that the `< 8` bit widths always
    // fill whole 64-bit words.
    buffer_size = (buffer_size + 63) & ~63;
    next_values_.assign(static_cast<size_t>(buffer_size), 0);
    const size_t next_blocks_size =
        (static_cast<size_t>(buffer_size) * static_cast<size_t>(bits_per_value)) / 8 + BitUtil::LONG_BYTES - 1;
    next_blocks_.assign(next_blocks_size, 0);
}

Result<DirectWriter> DirectWriter::get_instance(DataOutput &output, int64_t num_values, int32_t bits_per_value) {
    if (!std::binary_search(SUPPORTED_BITS_PER_VALUE.begin(), SUPPORTED_BITS_PER_VALUE.end(), bits_per_value)) {
        return err(LuceneError::illegal_argument("Unsupported bitsPerValue " + std::to_string(bits_per_value) +
                                                 ". Did you use bits_required?"));
    }
    return DirectWriter(output, num_values, bits_per_value);
}

Result<int32_t> DirectWriter::bits_required(int64_t max_value) {
    auto bits = PackedInts::bits_required(max_value);
    if (!bits.has_value()) {
        return bits;
    }
    // Round up to the next supported width.
    const auto it = std::lower_bound(SUPPORTED_BITS_PER_VALUE.begin(), SUPPORTED_BITS_PER_VALUE.end(), bits.value());
    if (it == SUPPORTED_BITS_PER_VALUE.end()) {
        return err(LuceneError::illegal_argument("Cannot represent values up to " + std::to_string(max_value)));
    }
    return *it;
}

int32_t DirectWriter::unsigned_bits_required(int64_t max_value) {
    const int32_t bits = PackedInts::unsigned_bits_required(max_value);
    const auto it = std::lower_bound(SUPPORTED_BITS_PER_VALUE.begin(), SUPPORTED_BITS_PER_VALUE.end(), bits);
    return it == SUPPORTED_BITS_PER_VALUE.end() ? SUPPORTED_BITS_PER_VALUE.back() : *it;
}

VoidResult DirectWriter::add(int64_t value) {
    debug_check(bits_per_value_ == 64 || (value >= 0 && value <= PackedInts::max_value(bits_per_value_)),
                "value does not fit bitsPerValue");
    debug_check(!finished_, "add() called after finish()");
    if (count_ >= num_values_) {
        return err(LuceneError::eof("Writing past end of stream"));
    }
    next_values_[off_] = value;
    ++off_;
    if (off_ == next_values_.size()) {
        if (auto result = flush(); !result.has_value()) {
            return result;
        }
    }
    ++count_;
    return {};
}

VoidResult DirectWriter::flush() {
    if (off_ == 0) {
        return {};
    }
    // Values past `off_` must not leak into the output.
    for (size_t i = off_; i < next_values_.size(); ++i) {
        next_values_[i] = 0;
    }
    encode(next_values_, off_, next_blocks_, bits_per_value_);
    const size_t block_count = packed_byte_count(PackedInts::VERSION_CURRENT, static_cast<int64_t>(off_),
                                                 bits_per_value_);
    if (auto result = output_->write_bytes(next_blocks_, 0, block_count); !result.has_value()) {
        return result;
    }
    off_ = 0;
    return {};
}

void DirectWriter::encode(std::span<const int64_t> values, size_t upto, std::span<uint8_t> blocks,
                          int32_t bits_per_value) {
    const size_t bpv = static_cast<size_t>(bits_per_value);
    if ((bpv & 7) == 0) {
        // Whole byte multiples: one little-endian integer per value.
        const size_t bytes_per_value = bpv / 8;
        size_t o = 0;
        for (size_t i = 0; i < upto; ++i) {
            const int64_t value = values[i];
            if (bpv > 32) {
                BitUtil::set_i64_le(blocks, o, value);
            } else if (bpv > 16) {
                BitUtil::set_i32_le(blocks, o, static_cast<int32_t>(value));
            } else if (bpv > 8) {
                BitUtil::set_i16_le(blocks, o, static_cast<int16_t>(value));
            } else {
                blocks[o] = static_cast<uint8_t>(value);
            }
            o += bytes_per_value;
        }
        return;
    }
    if (bpv < 8) {
        // 1, 2, 4 bits: `64 / bpv` values per little-endian 64-bit word.
        const size_t values_per_long = 64 / bpv;
        size_t i = 0;
        size_t o = 0;
        while (i < upto) {
            uint64_t word = 0;
            for (size_t j = 0; j < values_per_long; ++j) {
                word |= static_cast<uint64_t>(values[i + j]) << (bpv * j);
            }
            BitUtil::set_i64_le(blocks, o, static_cast<int64_t>(word));
            o += BitUtil::LONG_BYTES;
            i += values_per_long;
        }
        return;
    }
    // 12, 20 or 28 bits: two values merge into one little-endian integer.
    const size_t step = bpv / 4;
    size_t i = 0;
    size_t o = 0;
    while (i < upto) {
        const int64_t merged = values[i] | (values[i + 1] << bpv);
        if (bpv == 12) {
            BitUtil::set_i32_le(blocks, o, static_cast<int32_t>(merged));
        } else {
            BitUtil::set_i64_le(blocks, o, merged);
        }
        o += step;
        i += 2;
    }
}

VoidResult DirectWriter::finish() {
    if (count_ != num_values_) {
        return err(LuceneError::illegal_state("Wrong number of values added, expected: " + std::to_string(num_values_) +
                                              ", got: " + std::to_string(count_)));
    }
    debug_check(!finished_, "finish() called twice");
    if (auto result = flush(); !result.has_value()) {
        return result;
    }

    // Padding so that the last value can still be decoded together with the
    // bytes of its enclosing word/pair, e.g. 20-bit values are read with a
    // 32-bit read and therefore need 32 - 20 = 12 bits of padding.
    int32_t padding_bits_needed = 0;
    if (bits_per_value_ > 32) {
        padding_bits_needed = 64 - bits_per_value_;
    } else if (bits_per_value_ > 16) {
        padding_bits_needed = 32 - bits_per_value_;
    } else if (bits_per_value_ > 8) {
        padding_bits_needed = 16 - bits_per_value_;
    }
    debug_check(padding_bits_needed >= 0, "padding cannot be negative");
    const int32_t padding_bytes_needed = (padding_bits_needed + 7) / 8;
    debug_check(padding_bytes_needed <= 3, "at most 3 padding bytes");
    for (int32_t i = 0; i < padding_bytes_needed; ++i) {
        if (auto result = output_->write_byte(0); !result.has_value()) {
            return result;
        }
    }
    finished_ = true;
    return {};
}

} // namespace lucene
