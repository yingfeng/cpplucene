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

module lucene_core:growable_writer.impl;

import :growable_writer;
import :packed64;
import :packed_ints;
import :types;
import :error;
import :debug;

import std;

namespace lucene {

GrowableWriter::GrowableWriter(Packed64 current, float acceptable_overhead_ratio)
    : current_(std::move(current)),
      current_mask_(mask_for(current_.bits_per_value())),
      acceptable_overhead_ratio_(acceptable_overhead_ratio) {}

int64_t GrowableWriter::mask_for(int32_t bits_per_value) noexcept {
    return bits_per_value >= 64 ? -1 : PackedInts::max_value(bits_per_value);
}

Result<GrowableWriter> GrowableWriter::create(int32_t start_bits_per_value, size_t value_count,
                                              float acceptable_overhead_ratio) {
    // The start width is rounded like every other packed allocation, so that a
    // `1`-bit request can still end up as an 8-bit array for speed.
    const int32_t bits = fastest_format_and_bits(static_cast<int32_t>(value_count), start_bits_per_value,
                                                 acceptable_overhead_ratio)
                             .bits_per_value;
    auto current = Packed64::create(value_count, bits);
    if (!current.has_value()) {
        return err(current.error());
    }
    return GrowableWriter(std::move(current.value()), acceptable_overhead_ratio);
}

VoidResult GrowableWriter::ensure_capacity(int64_t value) {
    if ((value & current_mask_) == value) {
        return {};
    }
    const int32_t bits_required = PackedInts::unsigned_bits_required(value);
    debug_check(bits_required > current_.bits_per_value(), "growing must increase the bit width");
    const int32_t bits = fastest_format_and_bits(static_cast<int32_t>(current_.size()), bits_required,
                                                 acceptable_overhead_ratio_)
                             .bits_per_value;

    auto next = Packed64::create(current_.size(), bits);
    if (!next.has_value()) {
        return err(next.error());
    }
    if (auto result = next->copy_from(current_, 0, 0, current_.size()); !result.has_value()) {
        return result;
    }
    current_ = std::move(next.value());
    current_mask_ = mask_for(current_.bits_per_value());
    return {};
}

VoidResult GrowableWriter::set(size_t index, int64_t value) {
    if (auto result = ensure_capacity(value); !result.has_value()) {
        return result;
    }
    return current_.set(index, value);
}

VoidResult GrowableWriter::set_bulk(size_t index, std::span<const int64_t> values) {
    int64_t max = 0;
    for (const int64_t value : values) {
        max |= value;
    }
    if (auto result = ensure_capacity(max); !result.has_value()) {
        return result;
    }
    for (size_t i = 0; i < values.size(); ++i) {
        if (auto result = current_.set(index + i, values[i]); !result.has_value()) {
            return result;
        }
    }
    return {};
}

VoidResult GrowableWriter::fill(size_t from_index, size_t to_index, int64_t value) {
    if (auto result = ensure_capacity(value); !result.has_value()) {
        return result;
    }
    return current_.fill(from_index, to_index, value);
}

Result<GrowableWriter> GrowableWriter::resize(size_t new_size) const {
    auto next = Packed64::create(new_size, current_.bits_per_value());
    if (!next.has_value()) {
        return err(next.error());
    }
    const size_t limit = std::min(current_.size(), new_size);
    if (auto result = next->copy_from(current_, 0, 0, limit); !result.has_value()) {
        return err(result.error());
    }
    return GrowableWriter(std::move(next.value()), acceptable_overhead_ratio_);
}

} // namespace lucene
