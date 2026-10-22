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

export module lucene_core:stored_fields_format;

import :types;
import std;

import :directory;
import :error;
import :field_infos;
import :io_context;
import :segment_info;
import :stored_fields_reader;
import :stored_fields_writer;

namespace lucene {

/// Controls the format of the stored fields of a segment.
///
/// Port of the `StoredFieldsFormat` trait (`codecs/stored_fields_format.rs`).
export class StoredFieldsFormat {
public:
    virtual ~StoredFieldsFormat() = default;

    /// The name the codec metadata uses to select this format.
    [[nodiscard]] virtual std::string_view name() const = 0;

    /// A reader over the stored fields of `segment_info`.
    [[nodiscard]] virtual Result<std::unique_ptr<StoredFieldsReader>> fields_reader(
            const Directory &directory, const SegmentInfo &segment_info,
            std::shared_ptr<const FieldInfos> field_infos, const IOContext &context) const = 0;

    /// A writer for the stored fields of `segment_info`.
    /// Non-const `segment_info` because the format records the compression mode
    /// it used as a segment attribute, which a reader needs to reopen the
    /// segment (rlucene takes `&mut SegmentInfo` for the same reason).
    [[nodiscard]] virtual Result<std::unique_ptr<StoredFieldsWriter>> fields_writer(
            Directory &directory, SegmentInfo &segment_info, const IOContext &context) const = 0;
};

} // namespace lucene
