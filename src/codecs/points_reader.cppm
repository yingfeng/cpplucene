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

export module lucene_core:points_reader;

import :types;
import :error;

import std;

import :point_values;

namespace lucene {

/// Reads the indexed point values of a segment.
///
/// Port of the `PointsReader` trait (`codecs/points_reader.rs`): one instance per
/// segment, handing out the [`PointValues`] of each point field by name.
export class PointsReader {
public:
    virtual ~PointsReader() = default;

    /// Validates the checksums of this reader's files.
    [[nodiscard]] virtual VoidResult check_integrity() = 0;

    /// The point values of `field_name`, or `nullptr` when the reader holds none
    /// for it (the reference implementation's `Ok(None)`).
    ///
    /// Fails when the field is unknown or does not index points at all.
    [[nodiscard]] virtual Result<std::shared_ptr<PointValues>> get_values(std::string_view field_name) const = 0;

    /// `true` for the Lucene 9.0 points reader, which lets the points writer take
    /// its merge path; every other reader uses the generic one.
    [[nodiscard]] virtual bool is_lucene90_points_reader() const { return false; }

    /// Closes the files this reader opened.
    ///
    /// **Deviation:** the reference implementation gets this from the separate `Closeable` trait; the
    /// port keeps it on the reader so callers can release it deterministically
    /// instead of relying on the destructor.
    [[nodiscard]] virtual VoidResult close() = 0;
};

} // namespace lucene
