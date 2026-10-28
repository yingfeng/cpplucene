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

export module lucene_core:buffered_updates_stream;

import :error;
import :frozen_buffered_updates;
import :types;

import std;

namespace lucene {

/// The packets of frozen buffered updates, ordered by their deletion generation.
///
/// Port of `index/buffered_updates_stream.rs`, reduced to the accounting half:
/// the stream hands out generations, remembers which packets were applied and
/// forgets the ones that are done.  The reference implementation also keeps the
/// segment states it applies a packet to and enforces the "wait until every
/// packet below this generation is applied" contract; in this port the writer
/// applies a packet while it holds the segment lock, so there is never more than
/// one open packet per commit and no waiting is needed.
///
/// **Deviation:** applying a packet lives in `IndexWriterImpl` (which owns the
/// segment states), not here; the stream is the ledger that makes the generation
/// bookkeeping explicit and observable.
export class BufferedUpdatesStream {
public:
    BufferedUpdatesStream() = default;

    /// Registers `packet` and assigns it the next deletion generation.
    ///
    /// Returns the generation; the packet records it too, so it can be compared
    /// with a segment's `del_gen`.
    [[nodiscard]] Result<int64_t> push(const std::shared_ptr<FrozenBufferedUpdates> &packet);

    /// The generation that will be handed out next.
    [[nodiscard]] int64_t next_del_gen() const noexcept { return next_del_gen_; }

    /// Every packet up to and including this generation has been applied.
    [[nodiscard]] int64_t applied_gen() const noexcept { return applied_gen_; }

    /// Records that every packet up to `gen` is applied.
    void set_applied_gen(int64_t gen) noexcept {
        if (gen > applied_gen_) {
            applied_gen_ = gen;
        }
    }

    /// Number of packets still held (applied ones included).
    [[nodiscard]] size_t num_packets() const noexcept { return packets_.size(); }

    /// Number of packets that were not applied yet.
    [[nodiscard]] size_t num_unapplied_packets() const noexcept;

    /// Documents the held packets deleted or updated.
    [[nodiscard]] int64_t total_del_count() const noexcept;

    /// The per-packet summaries, oldest first.
    [[nodiscard]] std::vector<std::string> packets_to_string() const;

    /// Forgets the applied packets; the generation counters are kept.
    void prune_applied();

    /// Forgets every packet and resets the generations.
    void clear();

    [[nodiscard]] std::string to_string() const;

private:
    std::vector<std::shared_ptr<FrozenBufferedUpdates>> packets_;
    int64_t next_del_gen_{1};
    int64_t applied_gen_{0};
};

} // namespace lucene
