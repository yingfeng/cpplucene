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

module lucene_core:block_packed_writer.impl;

import :block_packed_writer;

import :bit_util;
import :data_output;
import :error;
import :packed_stream;
import :packed_ints;
import :types;

import std;

namespace lucene {

BlockPackedWriter::BlockPackedWriter(size_t block_size) : values_(block_size, 0), encoded_(block_size, 0) {}

Result<BlockPackedWriter> BlockPackedWriter::create(size_t block_size) {
    if (block_size < static_cast<size_t>(MIN_BLOCK_SIZE) ||
        block_size > static_cast<size_t>(MAX_BLOCK_SIZE) || block_size % 64 != 0) {
        return err(LuceneError::illegal_argument("block_size must be a multiple of 64 in [" +
                                                 std::to_string(MIN_BLOCK_SIZE) + ", " +
                                                 std::to_string(MAX_BLOCK_SIZE) + "] (got " +
                                                 std::to_string(block_size) + ")"));
    }
    return BlockPackedWriter(block_size);
}

void BlockPackedWriter::reset() noexcept {
    off_ = 0;
    ord_ = 0;
    finished_ = false;
}

VoidResult BlockPackedWriter::add(int64_t value, DataOutput &out) {
    if (finished_) {
        return err(LuceneError::illegal_state("Already finished"));
    }
    if (off_ == values_.size()) {
        if (auto written = flush(out); !written.has_value()) {
            return written;
        }
    }
    values_[off_] = value;
    ++off_;
    ++ord_;
    return {};
}

VoidResult BlockPackedWriter::finish(DataOutput &out) {
    if (finished_) {
        return err(LuceneError::illegal_state("Already finished"));
    }
    if (off_ > 0) {
        if (auto written = flush(out); !written.has_value()) {
            return written;
        }
    }
    finished_ = true;
    return {};
}

VoidResult BlockPackedWriter::write_vlong(DataOutput &out, int64_t value) {
    int64_t i = value;
    int32_t k = 0;
    while ((i & ~static_cast<int64_t>(0x7F)) != 0 && k < 8) {
        if (auto written = out.write_byte(static_cast<uint8_t>((i & 0x7F) | 0x80)); !written.has_value()) {
            return written;
        }
        i >>= 7;
        ++k;
    }
    return out.write_byte(static_cast<uint8_t>(i));
}

VoidResult BlockPackedWriter::flush(DataOutput &out) {
    int64_t min = std::numeric_limits<int64_t>::max();
    int64_t max = std::numeric_limits<int64_t>::min();
    for (size_t i = 0; i < off_; ++i) {
        min = std::min(min, values_[i]);
        max = std::max(max, values_[i]);
    }

    const int64_t delta = static_cast<int64_t>(static_cast<uint64_t>(max) - static_cast<uint64_t>(min));
    const int32_t bits_required = delta == 0 ? 0 : PackedInts::unsigned_bits_required(delta);

    int64_t min_adjusted = min;
    if (bits_required == 64) {
        min_adjusted = 0;
    } else if (min > 0) {
        const int64_t high = static_cast<int64_t>(static_cast<uint64_t>(max) -
                                                  static_cast<uint64_t>(PackedInts::max_value(bits_required)));
        min_adjusted = std::max<int64_t>(high, 0);
    }

    const int32_t token = (bits_required << BPV_SHIFT) | (min_adjusted == 0 ? MIN_VALUE_EQUALS_0 : 0);
    if (auto written = out.write_byte(static_cast<uint8_t>(token)); !written.has_value()) {
        return written;
    }

    if (min_adjusted != 0) {
        if (auto written = write_vlong(out, static_cast<int64_t>(static_cast<uint64_t>(
                                                    BitUtil::zig_zag_encode_i64(min_adjusted)) -
                                            1ULL));
            !written.has_value()) {
            return written;
        }
    }

    if (bits_required > 0) {
        for (size_t i = 0; i < off_; ++i) {
            encoded_[i] = static_cast<int64_t>(static_cast<uint64_t>(values_[i]) -
                                               static_cast<uint64_t>(min_adjusted));
        }
        if (auto written = write_packed_stream(out, std::span<const int64_t>(encoded_.data(), off_),
                                               bits_required);
            !written.has_value()) {
            return written;
        }
    }

    off_ = 0;
    return {};
}

} // namespace lucene
