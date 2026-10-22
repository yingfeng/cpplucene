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

export module lucene_core:growable_writer;

import :types;
import :error;

import std;

import :packed64;

namespace lucene {

/// A mutable packed array that grows its bit width on demand.
///
/// Port of `util/packed/growable_writer.rs` (Lucene's `GrowableWriter`), the
/// address allocator of the doc values writers.
///
/// Negative values are accepted, but storing one grows the array to 64 bits per
/// value.
export class GrowableWriter {
public:
    /// Creates a writer for `value_count` values starting at
    /// `start_bits_per_value`, rounded up per `acceptable_overhead_ratio`.
    [[nodiscard]] static Result<GrowableWriter> create(int32_t start_bits_per_value, size_t value_count,
                                                       float acceptable_overhead_ratio);

    [[nodiscard]] int64_t get(size_t index) const noexcept { return current_.get(index); }

    [[nodiscard]] VoidResult set(size_t index, int64_t value);
    /// Sets `values` starting at `index`; the array grows to fit the maximum.
    [[nodiscard]] VoidResult set_bulk(size_t index, std::span<const int64_t> values);
    [[nodiscard]] VoidResult fill(size_t from_index, size_t to_index, int64_t value);
    void clear() noexcept { current_.clear(); }

    [[nodiscard]] size_t size() const noexcept { return current_.size(); }
    [[nodiscard]] int32_t bits_per_value() const noexcept { return current_.bits_per_value(); }
    [[nodiscard]] const Packed64 &current() const noexcept { return current_; }

    /// A copy of this writer's values, resized to `new_size`.
    [[nodiscard]] Result<GrowableWriter> resize(size_t new_size) const;

private:
    GrowableWriter(Packed64 current, float acceptable_overhead_ratio);

    /// Grows the array so that `value` can be stored.
    [[nodiscard]] VoidResult ensure_capacity(int64_t value);

    [[nodiscard]] static int64_t mask_for(int32_t bits_per_value) noexcept;

    Packed64 current_;
    int64_t current_mask_;
    float acceptable_overhead_ratio_;
};

} // namespace lucene
