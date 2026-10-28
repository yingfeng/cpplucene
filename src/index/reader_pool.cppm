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

export module lucene_core:reader_pool;

import :directory;
import :error;
import :io_context;
import :readers_and_updates;
import :segment_commit_info;
import :segment_infos;
import :segment_reader;
import :types;

import std;

namespace lucene {

/// The writer's cache of segment readers, one [`ReadersAndUpdates`] per live
/// segment.
///
/// Port of `index/reader_pool.rs`, reduced to the part this port needs: the
/// reference implementation also parks doc values generations and merge readers
/// here, while the port keeps those in `SegmentCommitInfo` and in the merge
/// policy.  Because the cached object carries the segment's pending deletions,
/// the near-real-time read and the committed view share one bit set per segment.
export class ReaderPool final {
public:
    /// A pool that opens readers from `directory`.
    explicit ReaderPool(Directory &directory) : directory_(&directory) {}

    ~ReaderPool() = default;

    /// The state of `info`, reusing the cached one when it is still current.
    ///
    /// A cached state is stale when the segment's `delGen`, `maxDoc`,
    /// field infos generation or doc values generation changed, e.g. after a
    /// commit that applied deletions; it is dropped and reopened.
    [[nodiscard]] Result<std::shared_ptr<ReadersAndUpdates>> get_readers_and_updates(SegmentCommitInfo &info,
                                                                                     int32_t created_version_major,
                                                                                     const IOContext &context);

    /// The reader of `info`, reusing the cached one when it is still current.
    [[nodiscard]] Result<std::shared_ptr<SegmentReader>> get(SegmentCommitInfo &info, int32_t created_version_major,
                                                             const IOContext &context);

    /// The cached state of the segment called `segment_name`, or null.
    [[nodiscard]] std::shared_ptr<ReadersAndUpdates> find_readers_and_updates(std::string_view segment_name) const;

    /// The cached reader of the segment called `segment_name`, or null.
    [[nodiscard]] std::shared_ptr<SegmentReader> find(std::string_view segment_name) const;

    /// Whether a reader for `segment_name` is cached.
    [[nodiscard]] bool is_cached(std::string_view segment_name) const {
        return find_readers_and_updates(segment_name) != nullptr;
    }

    /// Forgets the reader of `info` (e.g. after a merge dropped the segment).
    void drop(const SegmentCommitInfo &info);
    /// Forgets every reader.
    void drop_all();
    /// Forgets every reader but the ones of the segments `infos` lists.
    void retain_only(const SegmentInfos &infos);

    /// Closes and forgets every cached reader; idempotent.
    [[nodiscard]] VoidResult close();

    [[nodiscard]] size_t num_cached() const noexcept { return readers_.size(); }
    [[nodiscard]] bool is_closed() const noexcept { return closed_; }

private:
    Directory *directory_;
    std::map<std::string, std::shared_ptr<ReadersAndUpdates>, std::less<>> readers_;
    bool closed_{false};
};

} // namespace lucene
