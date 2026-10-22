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

export module lucene_core:field_infos_format;

import :types;
import std;

import :directory;
import :error;
import :field_infos;
import :io_context;
import :segment_info;

namespace lucene {

/// Encodes and decodes the field infos of a segment (`.fnm`).
///
/// Port of the `FieldInfosFormat` trait (`core/codecs/field_infos_format.rs`).
///
/// **Deviation:** the segment name and id come from the [`SegmentInfo`], like
/// rlucene, but the port keeps them as a `const SegmentInfo &` instead of a
/// `SegmentInfo<D>`.
export class FieldInfosFormat {
public:
    virtual ~FieldInfosFormat() = default;

    /// Name of the format, as recorded in the `.fnm` header.
    [[nodiscard]] virtual std::string_view name() const = 0;

    /// Reads the field infos of `segment_info`, with the segment suffix
    /// `segment_suffix`.
    [[nodiscard]] virtual Result<FieldInfos> read(const Directory &directory, const SegmentInfo &segment_info,
                                                  std::string_view segment_suffix,
                                                  const IOContext &context) const = 0;

    /// Writes the field infos of `segment_info`.
    [[nodiscard]] virtual VoidResult write(Directory &directory, const SegmentInfo &segment_info,
                                           std::string_view segment_suffix, const FieldInfos &infos,
                                           const IOContext &context) const = 0;
};

/// The Lucene94 field infos format, the only one this port reads or writes.
///
/// Port of `Lucene94FieldInfosFormat`
/// (`core/codecs/lucene94/lucene94_field_infos_format.rs`), which is a unit
/// struct delegating to the static helpers of [`Lucene94FieldInfosFormat`] in
/// `lucene_core:field_infos`; the adapter exists so the codec can hand out a
/// [`FieldInfosFormat`].
export class Lucene94FieldInfosFormatAdapter final : public FieldInfosFormat {
public:
    [[nodiscard]] std::string_view name() const override { return Lucene94FieldInfosFormat::CODEC_NAME; }

    [[nodiscard]] Result<FieldInfos> read(const Directory &directory, const SegmentInfo &segment_info,
                                          std::string_view segment_suffix, const IOContext &context) const override;

    [[nodiscard]] VoidResult write(Directory &directory, const SegmentInfo &segment_info,
                                   std::string_view segment_suffix, const FieldInfos &infos,
                                   const IOContext &context) const override;
};

} // namespace lucene
