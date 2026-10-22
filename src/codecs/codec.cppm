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

export module lucene_core:codec;

import :types;
import :error;

import std;

import :live_docs_format;
import :segment_info_format;

namespace lucene {

/// A set of formats that together read and write one index format generation.
///
/// Port of the `Codec` trait (`codecs/codec.rs`).  rlucene keeps a registry of
/// codecs keyed by name so `segments_N` can pick the right one per segment; the
/// registry itself needs the whole index package, so it is not part of this
/// partition.  What matters here is the dispatch surface: every format hangs off
/// a codec name.
///
/// **Scope:** the metadata formats are ported so far — [`SegmentInfoFormat`],
/// the Lucene94 field infos format and [`LiveDocsFormat`].  The data formats
/// (postings, stored fields, norms, doc values, term vectors, points, vectors)
/// need the indexing pipeline (`index/`) to have anything to write, so they are
/// deliberately absent rather than stubbed.  Per the project scope, the HNSW
/// vector formats are not ported at all.
export class Codec {
public:
    virtual ~Codec() = default;

    /// Name recorded in `segments_N` and in every per-segment file header.
    [[nodiscard]] virtual std::string_view name() const = 0;

    /// The segment metadata format this codec writes.
    [[nodiscard]] virtual const SegmentInfoFormat &segment_info_format() const = 0;

    /// The deleted-documents format this codec writes.
    [[nodiscard]] virtual const LiveDocsFormat &live_docs_format() const = 0;

    /// Called once after construction, so a codec can register its formats.
    virtual void initialize() {}
};

} // namespace lucene
