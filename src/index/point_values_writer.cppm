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

export module lucene_core:point_values_writer;

import :types;
import std;

import :error;
import :field_infos;
import :points_writer;

namespace lucene {

/// Buffers the packed point values of one field until the segment is flushed.
///
/// Port of `PointValuesWriter` (`core/index/point_values_writer.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * the reference implementation streams the packed values through a
///    `PagedBytes`; this port keeps them in one flat buffer, which is the same
///    data in a single allocation;
///  * the reference implementation's optional `DocMap` (index sorting) is not
///    ported, because the port has no index sorter yet.
export class PointValuesWriter {
public:
    explicit PointValuesWriter(const FieldInfo &field_info);

    /// Buffers one point of `doc_id`; its length must be
    /// `point_dimension_count * point_num_bytes`.
    [[nodiscard]] VoidResult add_packed_value(int32_t doc_id, std::span<const uint8_t> value);

    /// How many documents contributed at least one point.
    [[nodiscard]] int32_t num_docs() const noexcept { return num_docs_; }

    /// How many points are buffered.
    [[nodiscard]] size_t num_points() const noexcept { return num_points_; }

    /// Hands the buffered values to `writer`, which copies them into the segment.
    ///
    /// The buffer is consumed: the writer is expected to be thrown away after a
    /// flush, exactly like the reference implementation's per-segment writer.
    [[nodiscard]] VoidResult flush(PointsWriter &writer);

private:
    const FieldInfo *field_info_;
    size_t packed_bytes_length_{0};
    std::vector<int32_t> doc_ids_;
    std::vector<uint8_t> bytes_;
    size_t num_points_{0};
    int32_t num_docs_{0};
    int32_t last_doc_id_{-1};
};

} // namespace lucene
