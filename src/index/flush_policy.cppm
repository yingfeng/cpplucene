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

export module lucene_core:flush_policy;

import :types;
import std;

namespace lucene {

/// What a [`FlushPolicy`] gets to look at when it decides whether the buffered
/// documents have to become a segment.
///
/// Port of the state the reference implementation reads through
/// `DocumentsWriterFlushControl` and `LiveIndexWriterConfig`
/// (`core/index/flush_policy.rs`).
///
/// **Deviation:** there is one buffer and no per-thread accounting, so a policy
/// sees the numbers of the active buffer instead of the totals of a writer pool,
/// and the buffered deletes are not part of the decision (the port applies them
/// at commit time and does not measure them).
export struct FlushPolicyContext {
    /// Documents buffered in the active buffer.
    int32_t num_docs_in_ram{0};
    /// Bytes of document data the active buffer holds.
    int64_t bytes_used_in_ram{0};
    /// [`IndexWriterConfig::max_buffered_docs`](lucene_core:index_writer_config);
    /// a value `<= 0` disables flushing by document count.
    int32_t max_buffered_docs{-1};
    /// [`IndexWriterConfig::ram_buffer_size_bytes`](lucene_core:index_writer_config);
    /// the maximum is what a disabled limit looks like.
    int64_t ram_buffer_size_bytes{std::numeric_limits<int64_t>::max()};
};

/// Decides when the buffered documents have to be written out as a segment.
///
/// Port of the `FlushPolicy` trait (`core/index/flush_policy.rs`).
///
/// The writer asks its policy before it adds a document; a policy that answers
/// `true` makes the writer flush first, so the document starts a new segment.
/// The reference implementation asks after indexing a document and marks the
/// per-thread buffer flush pending instead; both amount to the same set of
/// segments, because the buffer is flushed once it reaches the limit either way.
///
/// **Deviations:** no thread states and no flush queue, so a policy cannot mark a
/// buffer pending or stall the indexing threads; the flush happens inline.
export class FlushPolicy {
public:
    virtual ~FlushPolicy() = default;

    /// Whether the buffered documents have to be flushed before the next one is
    /// added.
    [[nodiscard]] virtual bool should_flush(const FlushPolicyContext &context) const = 0;
};

/// The default policy: flush when the document count or the buffered bytes reach
/// their limits.
///
/// Port of `FlushByRamOrCountsPolicy` (`core/index/flush_by_ram_or_counts_policy.rs`).
///
/// The document count wins when both limits are enabled, which is the order the
/// reference implementation checks them in.
export class FlushByRamOrCountsPolicy final : public FlushPolicy {
public:
    /// The shared instance every writer uses until a caller configures another
    /// policy.
    [[nodiscard]] static std::shared_ptr<const FlushByRamOrCountsPolicy> create();

    [[nodiscard]] bool should_flush(const FlushPolicyContext &context) const override;

    /// Whether the policy flushes on the document count.
    [[nodiscard]] static bool flushes_on_doc_count(const FlushPolicyContext &context) noexcept;
    /// Whether the policy flushes on the buffered bytes.
    [[nodiscard]] static bool flushes_on_ram(const FlushPolicyContext &context) noexcept;

private:
    FlushByRamOrCountsPolicy() = default;
};

} // namespace lucene
