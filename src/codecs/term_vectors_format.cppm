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

export module lucene_core:term_vectors_format;

import :types;
import std;

import :directory;
import :error;
import :field_infos;
import :io_context;
import :segment_info;
import :term_vectors_reader;
import :term_vectors_writer;

namespace lucene {

/// Controls the format of the term vectors of a segment.
///
/// Port of the `TermVectorsFormat` trait (`codecs/term_vectors_format.rs`).
export class TermVectorsFormat {
public:
    virtual ~TermVectorsFormat() = default;

    /// The name the codec metadata uses to select this format.
    [[nodiscard]] virtual std::string_view name() const = 0;

    /// A reader over the term vectors of `segment_info`.
    [[nodiscard]] virtual Result<std::unique_ptr<TermVectorsReader>> vectors_reader(
            const Directory &directory, const SegmentInfo &segment_info,
            std::shared_ptr<const FieldInfos> field_infos, const IOContext &context) const = 0;

    /// A writer for the term vectors of `segment_info`.
    [[nodiscard]] virtual Result<std::unique_ptr<TermVectorsWriter>> vectors_writer(
            Directory &directory, const SegmentInfo &segment_info, const IOContext &context) const = 0;
};

} // namespace lucene
