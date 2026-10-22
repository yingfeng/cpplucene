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

module lucene_core:direct_monotonic_writer.impl;

import :direct_monotonic_writer;
import :direct_writer;
import :types;
import :error;
import :debug;
import :bit_util;
import :array_util;
import :index_output;

import std;

namespace lucene {

namespace {

/// `wrapping_sub` for the signed 64-bit arithmetic of the block encoding.
int64_t wrapping_sub(int64_t a, int64_t b) noexcept {
    return static_cast<int64_t>(static_cast<uint64_t>(a) - static_cast<uint64_t>(b));
}

} // namespace

DirectMonotonicWriter::DirectMonotonicWriter(IndexOutput &meta_out, IndexOutput &data_out, int64_t num_values,
                                            size_t base_data_pointer, int32_t block_shift)
    : meta_(&meta_out),
      data_(&data_out),
      num_values_(num_values),
      base_data_pointer_(base_data_pointer),
      buffer_(static_cast<size_t>(std::min<int64_t>(num_values, int64_t{1} << block_shift)), 0) {}

Result<DirectMonotonicWriter> DirectMonotonicWriter::get_instance(IndexOutput &meta_out, IndexOutput &data_out,
                                                                 int64_t num_values, int32_t block_shift) {
    if (block_shift < MIN_BLOCK_SHIFT || block_shift > MAX_BLOCK_SHIFT) {
        return err(LuceneError::illegal_argument("blockShift must be in [" + std::to_string(MIN_BLOCK_SHIFT) + "-" +
                                                 std::to_string(MAX_BLOCK_SHIFT) + "], got " +
                                                 std::to_string(block_shift)));
    }
    if (num_values < 0) {
        return err(LuceneError::illegal_argument("numValues can't be negative, got " + std::to_string(num_values)));
    }
    const int64_t num_blocks = num_values == 0 ? 0 : ((num_values - 1) >> block_shift) + 1;
    if (num_blocks > static_cast<int64_t>(ArrayUtil::MAX_ARRAY_LENGTH)) {
        return err(LuceneError::illegal_argument(
            "blockShift is too low for the provided number of values: blockShift=" + std::to_string(block_shift) +
            ", numValues=" + std::to_string(num_values)));
    }
    auto pointer = data_out.get_file_pointer();
    if (!pointer.has_value()) {
        return err(pointer.error());
    }
    return DirectMonotonicWriter(meta_out, data_out, num_values, pointer.value(), block_shift);
}

VoidResult DirectMonotonicWriter::flush() {
    debug_check(buffer_size_ != 0, "flush() requires a non-empty buffer");

    // Average slope of this block, stored as a float like Lucene does.
    const int64_t numerator = wrapping_sub(buffer_[buffer_size_ - 1], buffer_[0]);
    const auto denominator = static_cast<double>(std::max<size_t>(1, buffer_size_ - 1));
    const auto avg_inc = static_cast<float>(static_cast<double>(numerator) / denominator);

    int64_t min = std::numeric_limits<int64_t>::max();
    for (size_t i = 0; i < buffer_size_; ++i) {
        const auto expected = static_cast<int64_t>(avg_inc * static_cast<float>(i));
        buffer_[i] = wrapping_sub(buffer_[i], expected);
        min = std::min(buffer_[i], min);
    }

    int64_t max_delta = 0;
    for (size_t i = 0; i < buffer_size_; ++i) {
        buffer_[i] = wrapping_sub(buffer_[i], min);
        // `|` needs no branching and also works when the subtraction overflowed.
        max_delta |= buffer_[i];
    }

    if (auto result = meta_->write_long(min); !result.has_value()) {
        return result;
    }
    if (auto result = meta_->write_int(BitUtil::float_to_int_bits(avg_inc)); !result.has_value()) {
        return result;
    }
    auto pointer = data_->get_file_pointer();
    if (!pointer.has_value()) {
        return err(pointer.error());
    }
    if (auto result = meta_->write_long(static_cast<int64_t>(pointer.value() - base_data_pointer_));
        !result.has_value()) {
        return result;
    }

    if (max_delta == 0) {
        // Every value equals the expected one: no data at all.
        if (auto result = meta_->write_byte(0); !result.has_value()) {
            return result;
        }
    } else {
        const int32_t bits_required = DirectWriter::unsigned_bits_required(max_delta);
        auto writer = DirectWriter::get_instance(*data_, static_cast<int64_t>(buffer_size_), bits_required);
        if (!writer.has_value()) {
            return err(writer.error());
        }
        for (size_t i = 0; i < buffer_size_; ++i) {
            if (auto result = writer->add(buffer_[i]); !result.has_value()) {
                return result;
            }
        }
        if (auto result = writer->finish(); !result.has_value()) {
            return result;
        }
        if (auto result = meta_->write_byte(static_cast<uint8_t>(bits_required)); !result.has_value()) {
            return result;
        }
    }
    buffer_size_ = 0;
    return {};
}

VoidResult DirectMonotonicWriter::add(int64_t value) {
    if (value < previous_) {
        return err(LuceneError::illegal_argument("Values do not come in order: " + std::to_string(previous_) + ", " +
                                                 std::to_string(value)));
    }
    if (buffer_size_ == buffer_.size()) {
        if (auto result = flush(); !result.has_value()) {
            return result;
        }
    }
    buffer_[buffer_size_] = value;
    ++buffer_size_;
    previous_ = value;
    ++count_;
    return {};
}

VoidResult DirectMonotonicWriter::finish() {
    if (count_ != num_values_) {
        return err(LuceneError::illegal_state("Wrong number of values added, expected: " + std::to_string(num_values_) +
                                              ", got: " + std::to_string(count_)));
    }
    if (finished_) {
        return err(LuceneError::illegal_state("#finish has been called already"));
    }
    if (buffer_size_ > 0) {
        if (auto result = flush(); !result.has_value()) {
            return result;
        }
    }
    finished_ = true;
    return {};
}

} // namespace lucene
