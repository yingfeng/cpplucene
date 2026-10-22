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

export module lucene_core:direct_monotonic_reader;

import :types;
import :error;

import std;

import :data_input;
import :random_access_input;
import :direct_reader;

namespace lucene {

/// The per-block metadata written by [`DirectMonotonicWriter`].
///
/// Port of `DirectMonotonicReader.Meta`; `load` reads one
/// `(long min, int avg, long offset, byte bitsPerValue)` tuple per block.
export struct DirectMonotonicMeta {
    int32_t block_shift{0};
    std::vector<int64_t> mins;
    std::vector<float> avgs;
    std::vector<int64_t> offsets;
    std::vector<int32_t> bpvs;

    [[nodiscard]] size_t num_blocks() const noexcept { return mins.size(); }

    /// Reads the metadata for `num_values` values split by `block_shift`.
    [[nodiscard]] static Result<DirectMonotonicMeta> load(DataInput &meta_in, int64_t num_values,
                                                          int32_t block_shift);
};

/// Retrieves values written by [`DirectMonotonicWriter`].
///
/// Port of `util/packed/direct_monotonic_reader.rs`.
export class DirectMonotonicReader {
public:
    DirectMonotonicReader(DirectMonotonicMeta meta, const RandomAccessInput &data);

    /// The value at `index`.
    [[nodiscard]] Result<int64_t> get(size_t index) const;

    /// Binary search in `[from_index, to_index)`; returns the index of `key`, or
    /// `-1 - insertion_point` when absent.
    [[nodiscard]] Result<int64_t> binary_search(int64_t from_index, int64_t to_index, int64_t key) const;

    [[nodiscard]] const DirectMonotonicMeta &meta() const noexcept { return meta_; }

private:
    /// Lower/upper bound of the value at `index` without touching the data.
    [[nodiscard]] Result<std::array<int64_t, 2>> get_bounds(size_t index) const;

    DirectMonotonicMeta meta_;
    std::vector<std::optional<DirectReader>> readers_;
};

} // namespace lucene
