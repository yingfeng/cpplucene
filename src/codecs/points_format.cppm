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

export module lucene_core:points_format;

import :types;
import std;

import :error;
import :points_reader;
import :points_writer;
import :segment_info;
import :segment_read_state;
import :segment_write_state;

namespace lucene {

/// Encodes and decodes indexed points.
///
/// Port of the `PointsFormat` trait (`core/codecs/points_format.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * the associated `PointsWriter` / `PointsReader` types collapse into the
///    concrete type-erased interfaces;
///  * the write state is passed by non-const reference, matching
///    `Lucene90PointsFormat::fields_writer`, which mutates it while writing
///    (the readers take a non-const state for the same reason: opening files
///    advances the IO accounting).
export class PointsFormat {
public:
    virtual ~PointsFormat() = default;

    /// A writer for a new segment.
    [[nodiscard]] virtual Result<std::unique_ptr<PointsWriter>> fields_writer(SegmentWriteState &state,
                                                                              const SegmentInfo &segment_info) const = 0;

    /// A reader for an existing segment.
    ///
    /// **NOTE**: by the time this returns, the implementation must hold open
    /// every file it needs; otherwise the files may be deleted by a concurrent
    /// merge.
    [[nodiscard]] virtual Result<std::unique_ptr<PointsReader>> fields_reader(SegmentReadState &state,
                                                                              const SegmentInfo &segment_info) const = 0;
};

} // namespace lucene
