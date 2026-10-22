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

export module lucene_core:compound_format;

import :types;
import std;

import :compound_directory;
import :directory;
import :error;
import :io_context;
import :segment_info;

namespace lucene {

/// Encodes/decodes compound files.
///
/// Port of the `CompoundFormat` trait (`core/codecs/compound_format.rs`).
export class CompoundFormat {
public:
    virtual ~CompoundFormat() = default;

    /// Returns a read-only view of the compound files of `si`.
    [[nodiscard]] virtual Result<std::unique_ptr<CompoundDirectory>> get_compound_reader(Directory &dir,
                                                                                        const SegmentInfo &si) const = 0;

    /// Packs the files referenced by `si` into the compound format.
    ///
    /// All files referenced by `si` must have their headers and footers written
    /// with [`CodecUtil::write_index_header`] and [`CodecUtil::write_footer`].
    [[nodiscard]] virtual VoidResult write(Directory &dir, const SegmentInfo &si, const IOContext &context) const = 0;
};

} // namespace lucene
