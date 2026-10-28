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

module lucene_core:readers_and_updates.impl;

import :readers_and_updates;

import std;

import :buffered_updates;
import :directory;
import :error;
import :io_context;
import :live_docs_format;
import :pending_deletes;
import :segment_commit_info;
import :segment_reader;
import :types;

namespace lucene {

Result<std::shared_ptr<ReadersAndUpdates>> ReadersAndUpdates::create(std::shared_ptr<SegmentReader> reader,
                                                                     const SegmentCommitInfo &info) {
    if (reader == nullptr) {
        return err(LuceneError::illegal_argument("a ReadersAndUpdates needs a reader"));
    }
    auto pending = PendingDeletes::from_reader(*reader, info);
    if (!pending.has_value()) {
        return std::unexpected(std::move(pending.error()));
    }
    std::shared_ptr<ReadersAndUpdates> state(new ReadersAndUpdates(
            std::move(reader), std::move(pending.value()), info.del_gen, info.max_doc(), info.field_infos_gen,
            info.doc_values_gen));
    return state;
}

Result<bool> ReadersAndUpdates::write_live_docs(const LiveDocsFormat &format, Directory &dir,
                                                SegmentCommitInfo &info, int32_t new_del_count,
                                                const IOContext &context) {
    auto written = pending_deletes_->write_live_docs(format, dir, info, new_del_count, context);
    if (!written.has_value()) {
        return std::unexpected(std::move(written.error()));
    }
    if (!written.value()) {
        return false;
    }
    // The cached reader still shows the deletions of the `.liv` it was opened
    // from, so it must not be handed out again: the pool reopens the segment (and
    // rebuilds the pending deletions from the fresh live docs).
    stale_ = true;
    return true;
}

} // namespace lucene
