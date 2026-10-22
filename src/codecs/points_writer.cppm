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

export module lucene_core:points_writer;

import std;

import :field_infos;
import :merge_state;
import :points_reader;

namespace lucene {

/// Writes the indexed point values of a segment.
///
/// Port of the `PointsWriter` trait (`codecs/points_writer.rs`).
///
/// **Deviation:** the reference implementation's `write_field` also takes the directory and the
/// segment info, because its BKD writer needs them for temporary files.  Here the
/// concrete writer already holds both, so `write_field` matches Lucene's Java
/// signature and only needs the field and the values to copy.
export class PointsWriter {
public:
    virtual ~PointsWriter() = default;

    /// Copies the points of `field_info` out of `reader` into this segment.
    [[nodiscard]] virtual VoidResult write_field(const FieldInfo &field_info, PointsReader &reader) = 0;

    /// Writes the trailing metadata.  Calling it twice fails.
    [[nodiscard]] virtual VoidResult finish() = 0;

    /// Closes the files this writer created.
    [[nodiscard]] virtual VoidResult close() = 0;

    /// Merges the points of every segment in `merge_state`, then calls
    /// [`finish`].
    ///
    /// Port of `default_merge`.
    [[nodiscard]] virtual VoidResult merge(const MergeState &merge_state);

    /// Merges one point field by presenting the per-segment point trees as one
    /// virtual reader and letting [`write_field`] copy it.
    [[nodiscard]] virtual VoidResult merge_one_field(const MergeState &merge_state, const FieldInfo &field_info);
};

} // namespace lucene
