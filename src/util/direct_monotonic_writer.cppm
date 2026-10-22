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

export module lucene_core:direct_monotonic_writer;

import :types;
import :error;

import std;

import :index_output;

namespace lucene {

export inline constexpr int32_t MIN_BLOCK_SHIFT = 2;
export inline constexpr int32_t MAX_BLOCK_SHIFT = 22;

/// Writes monotonically increasing integer sequences.
///
/// Port of `util/packed/direct_monotonic_writer.rs` (Lucene's
/// `DirectMonotonicWriter`).  The input is split into blocks of
/// `2^block_shift` values; per block the writer stores the minimum value, the
/// average slope as a `float`, the offset of the block's data in `data_out`
/// and the number of bits used, then encodes only `value - expected` with a
/// [`DirectWriter`].
///
/// The metadata is written *interleaved*, one `(long, int, long, byte)` tuple
/// per block, which is what `DirectMonotonicMeta::load` expects.
export class DirectMonotonicWriter {
public:
    /// Creates a writer for `num_values` values.
    [[nodiscard]] static Result<DirectMonotonicWriter> get_instance(IndexOutput &meta_out, IndexOutput &data_out,
                                                                   int64_t num_values, int32_t block_shift);

    /// Appends `value`; values must be non-decreasing.
    [[nodiscard]] VoidResult add(int64_t value);

    /// Must be called exactly once after all values have been added.
    [[nodiscard]] VoidResult finish();

private:
    DirectMonotonicWriter(IndexOutput &meta_out, IndexOutput &data_out, int64_t num_values, size_t base_data_pointer,
                          int32_t block_shift);

    [[nodiscard]] VoidResult flush();

    IndexOutput *meta_;
    IndexOutput *data_;
    int64_t num_values_;
    size_t base_data_pointer_;
    std::vector<int64_t> buffer_;
    size_t buffer_size_{0};
    int64_t count_{0};
    bool finished_{false};
    int64_t previous_{std::numeric_limits<int64_t>::min()};
};

} // namespace lucene
