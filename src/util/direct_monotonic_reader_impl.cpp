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

module lucene_core:direct_monotonic_reader.impl;

import :direct_monotonic_reader;
import :direct_reader;
import :types;
import :error;
import :debug;
import :bit_util;
import :data_input;
import :random_access_input;

import std;

namespace lucene {

namespace {

int64_t wrapping_add(int64_t a, int64_t b) noexcept {
    return static_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b));
}

} // namespace

Result<DirectMonotonicMeta> DirectMonotonicMeta::load(DataInput &meta_in, int64_t num_values, int32_t block_shift) {
    if (num_values < 0) {
        return err(LuceneError::illegal_argument("numValues can't be negative, got " + std::to_string(num_values)));
    }
    const int64_t num_blocks = num_values == 0 ? 0 : ((num_values - 1) >> block_shift) + 1;

    DirectMonotonicMeta meta;
    meta.block_shift = block_shift;
    meta.mins.reserve(static_cast<size_t>(num_blocks));
    meta.avgs.reserve(static_cast<size_t>(num_blocks));
    meta.offsets.reserve(static_cast<size_t>(num_blocks));
    meta.bpvs.reserve(static_cast<size_t>(num_blocks));

    for (int64_t i = 0; i < num_blocks; ++i) {
        auto min = meta_in.read_long();
        if (!min.has_value()) {
            return err(min.error());
        }
        auto avg = meta_in.read_int();
        if (!avg.has_value()) {
            return err(avg.error());
        }
        auto offset = meta_in.read_long();
        if (!offset.has_value()) {
            return err(offset.error());
        }
        auto bpv = meta_in.read_byte();
        if (!bpv.has_value()) {
            return err(bpv.error());
        }
        meta.mins.push_back(min.value());
        meta.avgs.push_back(std::bit_cast<float>(avg.value()));
        meta.offsets.push_back(offset.value());
        meta.bpvs.push_back(static_cast<int32_t>(bpv.value()));
    }
    return meta;
}

DirectMonotonicReader::DirectMonotonicReader(DirectMonotonicMeta meta, const RandomAccessInput &data)
    : meta_(std::move(meta)) {
    readers_.reserve(meta_.num_blocks());
    for (size_t i = 0; i < meta_.num_blocks(); ++i) {
        const int32_t bpv = meta_.bpvs[i];
        if (bpv == 0) {
            // Constant block: every delta is zero, no data was written.
            readers_.emplace_back(std::nullopt);
        } else {
            readers_.emplace_back(DirectReader(data, static_cast<size_t>(meta_.offsets[i]), bpv));
        }
    }
}

Result<std::array<int64_t, 2>> DirectMonotonicReader::get_bounds(size_t index) const {
    const auto block_shift = static_cast<size_t>(meta_.block_shift);
    const size_t block = index >> block_shift;
    const size_t block_index = index & ((size_t{1} << block_shift) - 1);
    if (block >= readers_.size()) {
        return err(LuceneError::illegal_argument("index is out of bounds"));
    }
    const int64_t lower_bound =
        wrapping_add(meta_.mins[block], static_cast<int64_t>(meta_.avgs[block] * static_cast<float>(block_index)));
    // `wrapping_shl` in the Rust port masks the shift count; a 64-bit block can
    // never overflow the bounds, which the check below turns into "unknown".
    const int64_t upper_bound =
        wrapping_add(wrapping_add(lower_bound, int64_t{1} << (meta_.bpvs[block] & 63)), -1);
    if (meta_.bpvs[block] == 64 || upper_bound < lower_bound) {
        return std::array<int64_t, 2>{std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()};
    }
    return std::array<int64_t, 2>{lower_bound, upper_bound};
}

Result<int64_t> DirectMonotonicReader::get(size_t index) const {
    const auto block_shift = static_cast<size_t>(meta_.block_shift);
    const size_t block = index >> block_shift;
    const size_t block_index = index & ((size_t{1} << block_shift) - 1);
    if (block >= readers_.size()) {
        return err(LuceneError::illegal_argument("index is out of bounds"));
    }

    int64_t delta = 0;
    if (readers_[block].has_value()) {
        auto value = readers_[block]->get(block_index);
        if (!value.has_value()) {
            return err(value.error());
        }
        delta = value.value();
    }
    const int64_t expected =
        wrapping_add(meta_.mins[block], static_cast<int64_t>(meta_.avgs[block] * static_cast<float>(block_index)));
    return wrapping_add(expected, delta);
}

Result<int64_t> DirectMonotonicReader::binary_search(int64_t from_index, int64_t to_index, int64_t key) const {
    if (from_index < 0 || from_index > to_index) {
        return err(LuceneError::illegal_argument("fromIndex=" + std::to_string(from_index) +
                                                 ", toIndex=" + std::to_string(to_index)));
    }
    int64_t lo = from_index;
    int64_t hi = to_index - 1;
    while (lo <= hi) {
        const int64_t mid = (lo + hi) >> 1;
        // Run as many iterations as possible on the metadata, since each real
        // read may hit a page fault.
        auto bounds = get_bounds(static_cast<size_t>(mid));
        if (!bounds.has_value()) {
            return err(bounds.error());
        }
        if (bounds.value()[1] < key) {
            lo = mid + 1;
        } else if (bounds.value()[0] > key) {
            hi = mid - 1;
        } else {
            auto value = get(static_cast<size_t>(mid));
            if (!value.has_value()) {
                return err(value.error());
            }
            if (value.value() < key) {
                lo = mid + 1;
            } else if (value.value() > key) {
                hi = mid - 1;
            } else {
                return mid;
            }
        }
    }
    return -1 - lo;
}

} // namespace lucene
