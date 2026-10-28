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

export module lucene_core:index_writer_observers;

import :types;
import std;

import :error;
import :info_stream;
import :leaf_reader;

namespace lucene {

/// The merging segments a [`IndexWriterEventListener`] is told about.
///
/// Port of `MergeStat` (`index/merge_policy.rs`).
///
/// **Deviation:** the reference's `MergeStat` carries live handles (identity,
/// completion, progress, a registration flag) because merges run on background
/// threads; this port merges synchronously, so the event only needs to name the
/// input segments and the merge generation.
export struct MergeStat {
    /// One input segment: its name and its 16 byte id.
    struct Segment {
        std::string name;
        std::vector<uint8_t> id;

        /// The id as lower case hex.
        [[nodiscard]] std::string id_hex() const;
    };

    std::vector<Segment> segments;
    /// The index generation the merge belongs to.
    int64_t merge_gen{0};

    [[nodiscard]] std::string to_string() const;
};

/// A callback listener for the key events of an `IndexWriter`.
///
/// Port of `IndexWriterEventListener` (`index/index_writer_event_listener.rs`).
/// Like the reference implementation this is an experimental API: it exists so
/// that an application can record how long a commit's merges take.
export class IndexWriterEventListener {
public:
    virtual ~IndexWriterEventListener() = default;

    /// Called before the merges of a full flush run.
    virtual void begin_merge_on_full_flush(std::span<const MergeStat> merge_states) = 0;

    /// Called once those merges are done (or have given up).
    virtual void end_merge_on_full_flush(std::span<const MergeStat> merge_states) = 0;

    [[nodiscard]] virtual std::string to_string() const { return "IndexWriterEventListener"; }
};

/// The listener used when none was configured.
export class NoOpIndexWriterEventListener final : public IndexWriterEventListener {
public:
    void begin_merge_on_full_flush(std::span<const MergeStat> /*merge_states*/) override {}
    void end_merge_on_full_flush(std::span<const MergeStat> /*merge_states*/) override {}
    [[nodiscard]] std::string to_string() const override { return "NoOpIndexWriterEventListener"; }
};

/// Warms the reader of a freshly merged segment before it becomes visible.
///
/// Port of `IndexReaderWarmer` (`index/index_writer.rs`).
export class IndexReaderWarmer {
public:
    virtual ~IndexReaderWarmer() = default;

    /// Touches the data structures of `reader` so that the first search does not
    /// pay for them.
    [[nodiscard]] virtual VoidResult warm(LeafReader &reader) = 0;
};

/// A warmer that only forces the data structures to be initialised.
///
/// Port of `SimpleMergedSegmentWarmer`
/// (`index/simple_merged_segment_warmer.rs`): it opens the terms, the doc values
/// and the norms of every field of the merged segment, and reports how many of
/// each it touched.
export class SimpleMergedSegmentWarmer final : public IndexReaderWarmer {
public:
    explicit SimpleMergedSegmentWarmer(InfoStreamMT info_stream = nullptr);

    [[nodiscard]] VoidResult warm(LeafReader &reader) override;

    /// Number of fields whose terms were opened by the last [`warm`].
    [[nodiscard]] int32_t indexed_fields() const noexcept { return indexed_fields_; }
    /// Number of fields whose doc values were opened by the last [`warm`].
    [[nodiscard]] int32_t doc_values_fields() const noexcept { return doc_values_fields_; }
    /// Number of fields whose norms were opened by the last [`warm`].
    [[nodiscard]] int32_t norms_fields() const noexcept { return norms_fields_; }

private:
    InfoStreamMT info_stream_;
    int32_t indexed_fields_{0};
    int32_t doc_values_fields_{0};
    int32_t norms_fields_{0};
};

} // namespace lucene
