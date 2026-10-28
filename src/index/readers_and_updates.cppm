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

export module lucene_core:readers_and_updates;

import :buffered_updates;
import :directory;
import :error;
import :io_context;
import :live_docs_format;
import :pending_deletes;
import :segment_commit_info;
import :segment_reader;
import :types;

import std;

namespace lucene {

/// The writer's per-segment state: an open reader, its pending deletions and the
/// deletes and updates buffered against it.
///
/// Port of `index/readers_and_updates.rs`, reduced to what the port's writer
/// needs: the reference implementation also owns the doc values generations and
/// the merge bookkeeping, which the port keeps in `SegmentCommitInfo` and in the
/// merge policy.
///
/// **Deviations:**
///  * the reference implementation counts references with `Arc` and hands out
///    `Arc` clones; this port shares through `std::shared_ptr`;
///  * `write_live_docs` records the new deletion generation on the object (like
///    the reference) but leaves `del_count`/`soft_del_count` to the caller,
///    because the port splits hard and soft deletions at commit time.
export class ReadersAndUpdates {
public:
    /// Wraps `reader`; the pending deletions start from the reader's live docs.
    [[nodiscard]] static Result<std::shared_ptr<ReadersAndUpdates>> create(std::shared_ptr<SegmentReader> reader,
                                                                           const SegmentCommitInfo &info);

    [[nodiscard]] const std::shared_ptr<SegmentReader> &reader() const noexcept { return reader_; }

    /// Replaces the reader, e.g. after the segment gained files.
    void set_reader(std::shared_ptr<SegmentReader> reader) { reader_ = std::move(reader); }

    [[nodiscard]] PendingDeletes &pending_deletes() noexcept { return *pending_deletes_; }
    [[nodiscard]] const PendingDeletes &pending_deletes() const noexcept { return *pending_deletes_; }

    [[nodiscard]] BufferedUpdates &buffered_updates() noexcept { return buffered_updates_; }
    [[nodiscard]] const BufferedUpdates &buffered_updates() const noexcept { return buffered_updates_; }

    /// The deletion generation this state was built from.
    [[nodiscard]] int64_t del_gen() const noexcept { return del_gen_; }

    /// Whether this state still describes `info`.
    ///
    /// A state is stale after its live docs were written: the cached reader still
    /// holds the deletions of the `.liv` it was opened from, so it must be
    /// reopened before it is handed out again.
    [[nodiscard]] bool is_current_for(const SegmentCommitInfo &info) const noexcept {
        return !stale_ && del_gen_ == info.del_gen && max_doc_ == info.max_doc() &&
               field_infos_gen_ == info.field_infos_gen && doc_values_gen_ == info.doc_values_gen;
    }

    /// Writes the pending deletions and records the new generation; `true` when
    /// something was written.
    [[nodiscard]] Result<bool> write_live_docs(const LiveDocsFormat &format, Directory &dir,
                                               SegmentCommitInfo &info, int32_t new_del_count,
                                               const IOContext &context);

    /// Forgets the pending deletions; the reader stays open.
    void drop_changes() noexcept { pending_deletes_->drop_changes(); }

private:
    ReadersAndUpdates(std::shared_ptr<SegmentReader> reader, std::shared_ptr<PendingDeletes> pending, int64_t del_gen,
                      int64_t max_doc, int64_t field_infos_gen, int64_t doc_values_gen)
        : reader_(std::move(reader)),
          pending_deletes_(std::move(pending)),
          del_gen_(del_gen),
          max_doc_(max_doc),
          field_infos_gen_(field_infos_gen),
          doc_values_gen_(doc_values_gen) {}

    std::shared_ptr<SegmentReader> reader_;
    std::shared_ptr<PendingDeletes> pending_deletes_;
    BufferedUpdates buffered_updates_;
    int64_t del_gen_{-1};
    bool stale_{false};
    int64_t max_doc_{0};
    int64_t field_infos_gen_{-1};
    int64_t doc_values_gen_{-1};
};

} // namespace lucene
