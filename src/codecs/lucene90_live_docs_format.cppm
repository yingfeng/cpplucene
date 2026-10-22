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

export module lucene_core:lucene90_live_docs_format;

import :types;
import :error;

import std;

import :bits;
import :data_input;
import :data_output;
import :directory;
import :fixed_bit_set;
import :io_context;
import :live_docs_format;
import :segment_commit_info;

namespace lucene {

/// Live docs format of the Lucene 9.0+ / 10 codec.
///
/// Port of `Lucene90LiveDocsFormat` (`codecs/lucene90_live_docs_format.rs`).  The
/// file is `<segment>_<base36 delGen>.liv`:
///
/// ```text
/// header   // CodecUtil::write_index_header("Lucene90LiveDocs", 0, id, base36(delGen))
/// long[]   // bits2words(maxDoc) little endian words, bit set == document alive
/// footer
/// ```
///
/// Only the current format version is written and read; an older header is
/// rejected as `index_format_too_old`.
export class Lucene90LiveDocsFormat : public LiveDocsFormat {
public:
    static constexpr std::string_view CODEC_NAME = "Lucene90LiveDocs";
    static constexpr int32_t VERSION_CURRENT = 0;
    /// Extension of the live docs file.
    static constexpr std::string_view EXTENSION = "liv";

    [[nodiscard]] std::string_view name() const override { return CODEC_NAME; }

    [[nodiscard]] Result<FixedBitSet> read_live_docs(const Directory &dir, const SegmentCommitInfo &info,
                                                     const IOContext &context) const override;

    [[nodiscard]] VoidResult write_live_docs(const Bits &bits, Directory &dir, const SegmentCommitInfo &info,
                                             int32_t new_del_count, const IOContext &context) const override;

    [[nodiscard]] VoidResult files(const SegmentCommitInfo &info, std::set<std::string> &files) const override;

    /// File name of the live docs of `info` at `generation`.
    [[nodiscard]] static std::string file_name(std::string_view segment, int64_t generation);

    /// Writes `bits` into `out` and returns the number of **deleted** documents.
    [[nodiscard]] static Result<int32_t> write_bits(DataOutput &out, const Bits &bits);
};

} // namespace lucene
