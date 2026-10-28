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

export module lucene_core:concurrent_merge_scheduler;

import :types;
import std;

import :error;
import :merge_policy;
import :merge_scheduler;
import :merge_trigger;

namespace lucene {

/// A scheduler that runs merges on background threads.
///
/// Port of `ConcurrentMergeScheduler` (`index/concurrent_merge_scheduler.rs`).
///
/// The merges a [`MergePolicy`] proposes are handed to a pool of workers; each
/// worker runs one merge at a time and reports the first error it hits to the
/// next [`wait_for_merges`].  A merge holds the writer lock only while it takes
/// its inputs and while it publishes the result, so the file I/O of a merge
/// overlaps with the indexing the caller does next.
///
/// **Deviations:** there is no stall control (`do_stall`/`maybe_stall`) and no
/// rate limiting, because this port has no separate flush thread to stall; and
/// there is no `MergeScheduler::Message` queue, so a scheduler takes its merges
/// straight from the [`MergeSource`].  One scheduler must not be shared between
/// two writers: it remembers the source it was first used with and rejects
/// another one.
export class ConcurrentMergeScheduler final : public MergeScheduler {
public:
    /// Creates a scheduler with `max_thread_count` workers (at least one).
    [[nodiscard]] static std::shared_ptr<ConcurrentMergeScheduler> create(int32_t max_thread_count = 1);

    ~ConcurrentMergeScheduler() override;

    ConcurrentMergeScheduler(const ConcurrentMergeScheduler &) = delete;
    ConcurrentMergeScheduler &operator=(const ConcurrentMergeScheduler &) = delete;

    [[nodiscard]] std::string to_string() const override { return "ConcurrentMergeScheduler"; }

    /// Hands every merge `source` has queued to the workers and returns.
    [[nodiscard]] VoidResult merge(MergeSource &source, MergeTrigger trigger) override;

    /// Blocks until every worker is idle, then reports what they hit.
    [[nodiscard]] VoidResult wait_for_merges() override;

    /// Waits for the workers and stops them.
    [[nodiscard]] VoidResult close() override;

    [[nodiscard]] int32_t max_thread_count() const noexcept;
    /// Sets how many merges may run at once.
    [[nodiscard]] VoidResult set_max_thread_count(int32_t count);

    /// How many merges are queued or running.
    [[nodiscard]] size_t pending_merge_count() const;

private:
    explicit ConcurrentMergeScheduler(int32_t max_thread_count);

    struct State;
    std::shared_ptr<State> state_;
};

} // namespace lucene
