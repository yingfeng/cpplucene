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

export module lucene_core:bkd_doc_ids_writer;

import :types;
import :error;

import std;

import :data_input;
import :data_output;

namespace lucene {

/// Compresses the doc ids of one BKD leaf block.
///
/// Port of `DocIdsWriter` (`util/bkd/doc_ids_writer.rs`).
///
/// Every encoding starts with a one-byte tag:
///
/// | tag | encoding |
/// |-----|----------|
/// | `-2` | continuous ids: one `vint` seed |
/// | `-1` | bitset |
/// | `16` | delta, 16 bits per value |
/// | `24` | 24 bits per value |
/// | `32` | raw 32-bit ids |
/// | `0`  | legacy delta vints (read only) |
///
/// **Encoding choice** (same heuristics as rlucene and Java Lucene):
///
/// * `-2` continuous ids, when the block is strictly ascending and spans
///   exactly `count` values;
/// * `-1` bit set, when it is strictly ascending and `span <= count * 16`
///   (`span = max - min + 1`), which is what keeps a sparse block from
///   inflating the file beyond what the ids themselves cost;
/// * `16` delta, when the span fits in 16 bits;
/// * `24` when every id fits in 24 bits; `32` otherwise.
///
/// Because `DataOutput::write_int` is little endian in this port (as it is in
/// rlucene), the multi-byte encodings here are self-consistent with rlucene but
/// not byte-compatible with a Java written leaf — the same trade-off rlucene
/// already made.  The one-byte and `vint` fields are identical either way.
export struct BkdDocIdsWriter {
    static constexpr int8_t CONTINUOUS_IDS = -2;
    static constexpr int8_t BITSET_IDS = -1;
    static constexpr int8_t DELTA_BPV_16 = 16;
    static constexpr int8_t BPV_24 = 24;
    static constexpr int8_t BPV_32 = 32;
    static constexpr int8_t LEGACY_DELTA_VINT = 0;

    /// Writes `doc_ids` (already in leaf order) and returns the tag used.
    [[nodiscard]] static Result<int8_t> write_doc_ids(std::span<const int32_t> doc_ids, DataOutput &out);

    /// Reads `count` doc ids previously written by [`write_doc_ids`].
    ///
    /// Every encoding `write_doc_ids` can emit is decodable, plus the legacy
    /// delta-vint form that only old indexes carry.  An unknown tag fails with
    /// `corrupt_index`, as does a bit set whose population disagrees with
    /// `count`.
    [[nodiscard]] static VoidResult read_ints(DataInput &input, size_t count, std::span<int32_t> doc_ids);
};

} // namespace lucene
