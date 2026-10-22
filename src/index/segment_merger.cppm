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

export module lucene_core:segment_merger;

import :types;
import std;

import :codec;
import :codec_reader;
import :directory;
import :error;
import :field_infos_builder;
import :info_stream;
import :io_context;
import :merge_state;
import :segment_info;
import :segment_read_state;
import :segment_write_state;

namespace lucene {

/// Combines two or more segments into a single new segment.
///
/// Port of `SegmentMerger` (`index/segment_merger.rs`).
///
/// The merger owns a [`MergeState`] built from the readers and writes every
/// per-field file of the new segment through the codec of
/// `segment_info.codec_name`, in this order: stored fields, norms, postings,
/// doc values, points, term vectors, and finally the field infos (`.fnm`).
/// The caller is responsible for writing `segments_N`, which is what registers
/// the new segment.
///
/// **Deviations:**
///  * there is no intra-merge `TaskExecutor`: every field is merged on the
///    calling thread (the reference implementation only splits work across threads when one is
///    supplied);
///  * `min_version` is not recomputed from the readers' metadata, because this
///    port's [`CodecReader`] does not expose it; the value the caller put into
///    `segment_info` is kept;
///  * vectors (HNSW) are not merged, as everywhere else in this port;
///  * the reference implementation wraps every step in `catch_unwind` and then closes the writer with
///    `IOUtils::use_or_suppress_caught_result`; here failures are `Result`s, so
///    each step simply records the first error, closes what it opened, and
///    reports that error.
export class SegmentMerger {
public:
    /// Creates a merger for `readers` that writes the segment described by
    /// `segment_info` into `directory`.
    ///
    /// `context` must be a merge [`IOContext`]; `segment_info.max_doc` is
    /// overwritten with the number of live documents of the inputs.
    [[nodiscard]] static Result<std::unique_ptr<SegmentMerger>> create(std::span<CodecReader *const> readers,
                                                                      SegmentInfo &segment_info,
                                                                      InfoStreamMT info_stream,
                                                                      Directory &directory,
                                                                      const IOContext &context);

    ~SegmentMerger();

    SegmentMerger(const SegmentMerger &) = delete;
    SegmentMerger &operator=(const SegmentMerger &) = delete;
    SegmentMerger(SegmentMerger &&) = delete;
    SegmentMerger &operator=(SegmentMerger &&) = delete;

    /// `true` when the merge would produce a segment with at least one document.
    [[nodiscard]] bool should_merge() const;

    /// Writes the merged segment.
    ///
    /// Fails with `illegal_state` when the merge would produce an empty
    /// segment; otherwise every file of the new segment exists afterwards.
    [[nodiscard]] VoidResult merge();

    /// The merge state (doc maps, per-reader producers, field infos).
    [[nodiscard]] MergeState &merge_state() noexcept { return *merge_state_; }
    [[nodiscard]] const MergeState &merge_state() const noexcept { return *merge_state_; }

    /// Identity the caller should record for the new segment.
    [[nodiscard]] const std::vector<uint8_t> &id() const noexcept { return id_; }

private:
    SegmentMerger(std::unique_ptr<MergeState> merge_state, Directory &directory, const IOContext &context,
                  std::vector<uint8_t> id);

    /// Resolves the codec of the segment being written, or reports why it cannot.
    [[nodiscard]] Result<const Codec *> codec() const;

    [[nodiscard]] VoidResult merge_field_infos();
    [[nodiscard]] VoidResult merge_field_infos_with_state();

    [[nodiscard]] Result<int32_t> merge_fields();
    [[nodiscard]] VoidResult merge_norms();
    [[nodiscard]] VoidResult merge_terms(const SegmentWriteState &write_state, const SegmentReadState &read_state);
    [[nodiscard]] VoidResult merge_doc_values(const SegmentWriteState &write_state);
    [[nodiscard]] VoidResult merge_points(SegmentWriteState &write_state);
    [[nodiscard]] Result<int32_t> merge_term_vectors();

    std::unique_ptr<MergeState> merge_state_;
    Directory &directory_;
    const IOContext &context_;
    /// Collects the field infos of the merged segment.
    FieldInfosBuilder field_infos_builder_;
    std::vector<uint8_t> id_;
};

} // namespace lucene
