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

export module lucene_core:per_field_doc_values_format;

import :types;
import std;

import :doc_values_consumer;
import :doc_values_format;
import :doc_values_producer;
import :segment_info;
import :segment_read_state;
import :segment_write_state;

namespace lucene {

/// Name of this [`DocValuesFormat`].
///
/// Port of `codecs/perfield/per_field_doc_values_format.rs`.
export inline constexpr std::string_view PER_FIELD_DV_NAME = "PerFieldDV40";

/// `FieldInfo` attribute holding the doc values format name of each field.
export inline constexpr std::string_view PER_FIELD_DV_FORMAT_KEY = "PerFieldDocValuesFormat.format";

/// `FieldInfo` attribute holding the segment suffix of each field.
export inline constexpr std::string_view PER_FIELD_DV_SUFFIX_KEY = "PerFieldDocValuesFormat.suffix";

/// Enables per-field doc values support.
///
/// Every field records the doc values format that wrote it in the `.fnm`, and
/// the files of that format get an extra suffix, so a segment written by this
/// format stores `_0_Lucene90_0.dvd` rather than `_0.dvd`.  This is also what
/// makes in-place doc values updates possible: an updated field keeps the format
/// it was written with, at a later generation.
///
/// **Deviations** (C++ mechanics only):
///  * the reference implementation resolves the recorded format name through
///    `DocValuesFormat::for_name`; this port has no format registry (see
///    [`DocValuesFormat`]), so the recorded name is checked and then the codec's
///    default format is used, which is the only one a
///    [`PerFieldDocValuesFormat`] can hand out anyway;
///  * the write state the consumer needs is captured when the consumer is
///    created, because this port's `DocValuesConsumer::add_*_field` does not
///    take the state (see [`DocValuesConsumer`]).
export class PerFieldDocValuesFormat final : public DocValuesFormat {
public:
    /// Wraps `default_format`, which must outlive this object.
    explicit PerFieldDocValuesFormat(const DocValuesFormat &default_format) noexcept
            : default_format_(&default_format) {}

    [[nodiscard]] std::string_view name() const override { return PER_FIELD_DV_NAME; }

    [[nodiscard]] Result<std::unique_ptr<DocValuesConsumer>> fields_consumer(const SegmentWriteState &state,
                                                                             const SegmentInfo &segment_info) const override;

    [[nodiscard]] Result<std::unique_ptr<DocValuesProducer>> fields_producer(const SegmentReadState &state,
                                                                             const SegmentInfo &segment_info) const override;

private:
    const DocValuesFormat *default_format_;
};

} // namespace lucene
