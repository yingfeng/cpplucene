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

export module lucene_core:bkd_point_writer;

import :types;
import :error;

import std;

import :bkd_heap_point_writer;
import :bkd_offline_point;

namespace lucene {

/// Either sort of point sink the BKD writer builds a tree from.
///
/// Port of `PointWriterEnum` (`util/bkd/point_writer.rs`).  rlucene wraps both
/// writers in an enum whose `PointWriter` impl dispatches by variant; C++ gets
/// the same dispatch from a `std::variant` of pointers plus the free functions
/// in [`PointWriters`], which keeps the two writers independent types (no shared
/// base class, no virtual calls on the hot append path).
export using PointWriterVariant = std::variant<HeapPointWriter *, OfflinePointWriter *>;

/// The subset of the `PointWriter` trait the BKD writer actually uses.
export struct PointWriters {
    /// Appends one point given its packed dimensions and doc id.
    [[nodiscard]] static VoidResult append_bytes(PointWriterVariant writer, std::span<const uint8_t> packed_value,
                                                 int32_t doc_id);

    /// Appends a record that already contains the doc id.
    [[nodiscard]] static VoidResult append_packed(PointWriterVariant writer, std::span<const uint8_t> record);

    /// Number of points written so far.
    [[nodiscard]] static size_t count(const PointWriterVariant &writer);

    /// `true` if the points live in memory (and can be partitioned in place).
    [[nodiscard]] static bool is_on_heap(const PointWriterVariant &writer);

    /// The heap writer, or `nullptr` for the offline flavour.
    [[nodiscard]] static HeapPointWriter *heap_writer(const PointWriterVariant &writer);

    /// The offline writer, or `nullptr` for the heap flavour.
    [[nodiscard]] static OfflinePointWriter *offline_writer(const PointWriterVariant &writer);
};

} // namespace lucene
