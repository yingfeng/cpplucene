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

export module lucene_core:pending_deletes;

import :bits;
import :directory;
import :error;
import :fixed_bit_set;
import :io_context;
import :live_docs_format;
import :segment_commit_info;
import :segment_reader;
import :types;

import std;

namespace lucene {

/// Accounting and application of the pending deletes of one segment.
///
/// Port of `index/pending_deletes.rs`.  The live docs start out as the segment's
/// committed `.liv` (or as "everything is live" when it has no deletions) and are
/// only made writable when the first delete arrives, so a segment nobody deletes
/// from never materialises a bit set.
///
/// **Deviations:**
///  * the reference implementation identifies the segment by `SegmentInfo::id` and
///    keeps the live docs in a `CodecLiveDocsBits` wrapper; here the identity is
///    the segment name and the bits are a plain [`FixedBitSet`], because the port
///    has no codec-level live docs wrapper;
///  * [`write_live_docs`](Self::write_live_docs) takes the number of deletions to
///    record from the caller: the port's writer splits hard and soft deletions at
///    commit time (the reference implementation does that in
///    `PendingSoftDeletes` by overriding this method).
export class PendingDeletes {
public:
    /// Reads the segment's committed live docs, if it has any.
    [[nodiscard]] static Result<std::unique_ptr<PendingDeletes>> create(const LiveDocsFormat &format,
                                                                        const Directory &dir,
                                                                        const SegmentCommitInfo &info,
                                                                        const IOContext &context);

    /// Adopts the live docs of an open reader.
    [[nodiscard]] static Result<std::unique_ptr<PendingDeletes>> from_reader(const SegmentReader &reader,
                                                                             const SegmentCommitInfo &info);

    /// A segment whose live docs are `live_docs`; `initialized` says whether they
    /// are known (a segment without deletions starts out initialized but empty).
    [[nodiscard]] static Result<std::unique_ptr<PendingDeletes>> with(std::string segment_name,
                                                                      std::optional<FixedBitSet> live_docs,
                                                                      bool initialized, int32_t max_doc);

    [[nodiscard]] const std::string &segment_name() const noexcept { return segment_name_; }

    /// Deletes `doc_id`; `true` when it was live before.
    [[nodiscard]] Result<bool> delete_doc(int32_t doc_id);

    /// Deletes every document, without counting them one by one.
    void delete_all();

    /// Whether `doc_id` is deleted; `false` while the live docs are not known.
    [[nodiscard]] Result<bool> is_deleted(int32_t doc_id) const;

    /// Number of documents deleted since the live docs were last written.
    [[nodiscard]] int32_t num_pending_deletes() const noexcept { return pending_delete_count_; }

    [[nodiscard]] int32_t max_doc() const noexcept { return max_doc_; }

    /// Whether the live docs are known (a fresh segment without deletions is
    /// initialized but holds no bit set until the first delete).
    [[nodiscard]] bool live_docs_initialized() const noexcept { return live_docs_initialized_; }

    /// The live docs, or null while they are not known / not materialised yet.
    [[nodiscard]] const FixedBitSet *live_docs() const noexcept {
        return live_docs_.has_value() ? &live_docs_.value() : nullptr;
    }

    /// Number of live documents; `max_doc` when nothing is deleted.
    [[nodiscard]] int32_t num_live_docs() const noexcept {
        return live_docs_.has_value() ? static_cast<int32_t>(live_docs_->cardinality()) : max_doc_;
    }

    /// Adopts `reader`'s live docs when they are not known yet.
    [[nodiscard]] VoidResult on_new_reader(const SegmentReader &reader);

    /// Forgets the pending deletions without touching the committed live docs.
    void drop_changes() noexcept { pending_delete_count_ = 0; }

    /// Writes the live docs as `info`'s next `.liv` generation, recording
    /// `new_del_count` as the number of deletions this write adds.
    ///
    /// Returns `false` when nothing is pending.
    [[nodiscard]] Result<bool> write_live_docs(const LiveDocsFormat &format, Directory &dir,
                                               const SegmentCommitInfo &info, int32_t new_del_count,
                                               const IOContext &context);

private:
    PendingDeletes(std::string segment_name, std::optional<FixedBitSet> live_docs, bool initialized,
                   int32_t max_doc)
        : segment_name_(std::move(segment_name)),
          live_docs_(std::move(live_docs)),
          live_docs_initialized_(initialized),
          max_doc_(max_doc) {}

    /// The writable live docs, materialized on demand.
    [[nodiscard]] Result<FixedBitSet *> mutable_bits();

    std::string segment_name_;
    std::optional<FixedBitSet> live_docs_;
    int32_t pending_delete_count_{0};
    bool live_docs_initialized_{false};
    int32_t max_doc_{0};
};

} // namespace lucene
