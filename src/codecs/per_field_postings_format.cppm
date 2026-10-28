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

export module lucene_core:per_field_postings_format;

import :types;
import std;

import :error;
import :fields_consumer;
import :fields_producer;
import :postings_format;
import :segment_info;
import :segment_read_state;
import :segment_write_state;

namespace lucene {

/// Name of this [`PostingsFormat`].
///
/// Port of `codecs/perfield/per_field_postings_format.rs`.
export inline constexpr std::string_view PER_FIELD_POSTINGS_NAME = "PerField40";

/// `FieldInfo` attribute holding the postings format name of each field.
export inline constexpr std::string_view PER_FIELD_POSTINGS_FORMAT_KEY = "PerFieldPostingsFormat.format";

/// `FieldInfo` attribute holding the segment suffix of each field.
export inline constexpr std::string_view PER_FIELD_POSTINGS_SUFFIX_KEY = "PerFieldPostingsFormat.suffix";

/// Enables per-field postings support.
///
/// The name of the format used for a field is recorded in the segment's `.fnm`,
/// and every file a format writes gets an extra suffix built from that name and
/// a per-format counter, so a segment written by this format stores
/// `_0_Lucene101_0.tim` rather than `_0.tim`.
///
/// **Deviations** (C++ mechanics only):
///  * the reference implementation looks the format up again by name through
///    `PostingsFormat::for_name` when reading; this port has no format registry
///    (see [`PostingsFormat`]), so reading goes through the codec's default
///    format, which is the only one a [`PerFieldPostingsFormat`] can hand out
///    anyway;
///  * fields are grouped by format **name** instead of by format identity: the
///    base always returns the same instance, so the two are equivalent here.
export class PerFieldPostingsFormat final : public PostingsFormat {
public:
    /// Wraps `default_format`, which must outlive this object.
    explicit PerFieldPostingsFormat(const PostingsFormat &default_format) noexcept
            : default_format_(&default_format) {}

    [[nodiscard]] std::string_view name() const override { return PER_FIELD_POSTINGS_NAME; }

    [[nodiscard]] Result<std::unique_ptr<FieldsConsumer>> fields_consumer(const SegmentWriteState &state,
                                                                         const SegmentInfo &segment_info) const override;

    [[nodiscard]] Result<std::unique_ptr<FieldsProducer>> fields_producer(const SegmentReadState &state,
                                                                         const SegmentInfo &segment_info) const override;

private:
    const PostingsFormat *default_format_;
};

} // namespace lucene
