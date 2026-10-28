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

export module lucene_core:merge_scheduler;

import :types;
import std;

import :error;
import :merge_policy;
import :merge_trigger;

namespace lucene {

/// What a [`MergeScheduler`] pulls merges from.
///
/// Port of the `MergeSource` trait (`index/merge_scheduler.rs`).
///
/// **Deviations:** there is no `on_merge_finished` hook and no merge statistics,
/// because this port has no `MergeStat`/listener machinery; the source is also
/// not required to be thread safe, since no scheduler runs merges on another
/// thread yet.
export class MergeSource {
public:
    virtual ~MergeSource() = default;

    /// The next merge the policy asked for, or `nullopt` when none is queued.
    [[nodiscard]] virtual Result<std::optional<OneMerge>> get_next_merge() = 0;

    /// Whether merges are waiting to be executed.
    [[nodiscard]] virtual Result<bool> has_pending_merges() = 0;

    /// Merges the given segments, replacing them in the index by the result.
    [[nodiscard]] virtual VoidResult merge(OneMerge merge) = 0;
};

/// Decides how the merges a [`MergePolicy`] asks for are executed.
///
/// Port of the `MergeScheduler` trait (`index/merge_scheduler.rs`).
///
/// **Deviations:** no directory wrapping (`wrap_for_merge`, used for rate
/// limiting) and no intra-merge executor, so a merge is always single threaded.
export class MergeScheduler {
public:
    virtual ~MergeScheduler() = default;

    MergeScheduler(const MergeScheduler &) = delete;
    MergeScheduler &operator=(const MergeScheduler &) = delete;

    [[nodiscard]] virtual std::string to_string() const = 0;

    /// Runs the merges `source` hands out, until it has no more.
    [[nodiscard]] virtual VoidResult merge(MergeSource &source, MergeTrigger trigger) = 0;

    /// Waits for the merges this scheduler started in the background.
    ///
    /// A scheduler that runs its merges on the calling thread returns at once;
    /// a background scheduler blocks until its workers are idle, and reports the
    /// first error a worker hit.
    [[nodiscard]] virtual VoidResult wait_for_merges() { return {}; }

    /// Releases the scheduler; merges still running are waited for.
    [[nodiscard]] virtual VoidResult close() { return {}; }

protected:
    MergeScheduler() = default;
};

/// A scheduler that never runs a merge.
///
/// Port of `NoMergeScheduler`: use it together with [`NoMergePolicy`] to make
/// sure no merge ever happens.
export class NoMergeScheduler final : public MergeScheduler {
public:
    [[nodiscard]] static std::shared_ptr<NoMergeScheduler> create();

    [[nodiscard]] std::string to_string() const override { return "NoMergeScheduler"; }

    [[nodiscard]] VoidResult merge(MergeSource & /*source*/, MergeTrigger /*trigger*/) override { return {}; }
};

/// A scheduler that runs the merges one after another on the calling thread.
///
/// Port of `SerialMergeScheduler`.  This is the default of this port, because
/// the writer itself is single threaded (the reference implementation defaults
/// to `ConcurrentMergeScheduler`).
export class SerialMergeScheduler final : public MergeScheduler {
public:
    [[nodiscard]] static std::shared_ptr<SerialMergeScheduler> create();

    [[nodiscard]] std::string to_string() const override { return "SerialMergeScheduler"; }

    [[nodiscard]] VoidResult merge(MergeSource &source, MergeTrigger trigger) override;
};

} // namespace lucene
