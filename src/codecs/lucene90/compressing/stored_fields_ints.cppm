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

export module lucene_core:stored_fields_ints;

import :types;
import std;

import :data_output;
import :error;
import :index_input;

namespace lucene {

/// Encodes the per-document counts of a stored fields chunk.
///
/// Port of `StoredFieldsInts`
/// (`codecs/lucene90/compressing/stored_fields_ints.rs`).  A chunk stores two
/// arrays — the number of stored fields and the serialized length of every
/// document — as 8, 16 or 32 bits per value, transposed in blocks of 128 values
/// so that the low bytes of many values land in the same `long` (which is what
/// makes the arrays compress well), with a one-value shortcut for constant
/// arrays.
export class StoredFieldsInts {
public:
    /// Values per block.
    static constexpr size_t BLOCK_SIZE = 128;

    /// Writes `count` values of `values[start..]`.
    [[nodiscard]] static VoidResult write_ints(const std::vector<int32_t> &values, size_t start, size_t count,
                                               DataOutput &out);

    /// Reads `count` values into `values[offset..]`.
    [[nodiscard]] static VoidResult read_ints(IndexInput &input, size_t count, std::vector<int64_t> &values,
                                              size_t offset);

private:
    static VoidResult write_ints8(DataOutput &out, size_t count, const std::vector<int32_t> &values, size_t offset);
    static VoidResult write_ints16(DataOutput &out, size_t count, const std::vector<int32_t> &values, size_t offset);
    static VoidResult write_ints32(DataOutput &out, size_t count, const std::vector<int32_t> &values, size_t offset);

    static VoidResult read_ints8(IndexInput &input, size_t count, std::vector<int64_t> &values, size_t offset);
    static VoidResult read_ints16(IndexInput &input, size_t count, std::vector<int64_t> &values, size_t offset);
    static VoidResult read_ints32(IndexInput &input, size_t count, std::vector<int64_t> &values, size_t offset);
};

} // namespace lucene
