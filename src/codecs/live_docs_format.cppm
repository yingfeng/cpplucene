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

export module lucene_core:live_docs_format;

import :types;
import :error;

import std;

import :bits;
import :directory;
import :fixed_bit_set;
import :io_context;
import :segment_commit_info;

namespace lucene {

/// Reads and writes the deleted-documents bitmap of a segment.
///
/// Port of the `LiveDocsFormat` trait (`codecs/live_docs_format.rs`).  The file
/// is per segment and per deletion generation, and it lives outside compound
/// files because a commit can add deletions without rewriting the segment.
export class LiveDocsFormat {
public:
    virtual ~LiveDocsFormat() = default;

    /// Codec name recorded in the file header.
    [[nodiscard]] virtual std::string_view name() const = 0;

    /// Reads the bitmap of `info`'s current deletion generation.
    [[nodiscard]] virtual Result<FixedBitSet> read_live_docs(const Directory &dir, const SegmentCommitInfo &info,
                                                             const IOContext &context) const = 0;

    /// Writes the bitmap as `info`'s next deletion generation.
    ///
    /// `new_del_count` is how many deletions this write adds, so the format can
    /// verify the bitmap against the commit's bookkeeping.
    [[nodiscard]] virtual VoidResult write_live_docs(const Bits &bits, Directory &dir, const SegmentCommitInfo &info,
                                                     int32_t new_del_count, const IOContext &context) const = 0;

    /// Adds the live docs file of `info` to `files`, when it has one.
    [[nodiscard]] virtual VoidResult files(const SegmentCommitInfo &info, std::set<std::string> &files) const = 0;
};

} // namespace lucene
