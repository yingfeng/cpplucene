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

export module lucene_core:segment_info_format;

import :types;
import :error;

import std;

import :directory;
import :io_context;
import :segment_info;

namespace lucene {

/// Encodes and decodes the per-segment metadata file (`.si`).
///
/// Port of the `SegmentInfoFormat` trait (`codecs/segment_info_format.rs`).  The
/// trait becomes a base class so a [`Codec`] can hand out its format
/// polymorphically; there is exactly one implementation per codec version.
export class SegmentInfoFormat {
public:
    virtual ~SegmentInfoFormat() = default;

    /// Codec name recorded in the file header, e.g. `Lucene90SegmentInfo`.
    [[nodiscard]] virtual std::string_view name() const = 0;

    /// Writes `<info.name>.si`, adding the file name to `info.files`.
    ///
    /// `info` is mutable because writing the file makes it part of the segment,
    /// which is what rlucene does too (`si.add_file(file_name)`).
    [[nodiscard]] virtual VoidResult write(Directory &dir, SegmentInfo &info, const IOContext &context) const = 0;

    /// Reads `<segment>.si`, validating the header against `id`.
    [[nodiscard]] virtual Result<SegmentInfo> read(const Directory &dir, std::string_view segment,
                                                   std::span<const uint8_t> id, const IOContext &context) const = 0;
};

} // namespace lucene
