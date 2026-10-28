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

export module lucene_core:index_writer_config;

import :types;
import std;

import :analyzer;
import :codec;
import :flush_policy;
import :index_deletion_policy;
import :index_writer_observers;
import :merge_policy;
import :merge_scheduler;
import :similarity;
import :sort_field;

namespace lucene {

/// A sensible pool size for a machine: one buffer per core, between two and eight.
///
/// `IndexWriterConfig::set_max_indexing_buffers` does not default to it (a single
/// buffer keeps the documented flush thresholds), but a caller that wants parallel
/// indexing can pass it.
[[nodiscard]] size_t default_indexing_buffers() noexcept;

export class Query;

/// What [`IndexWriter::open`](lucene_core:index_writer) does when the directory
/// already holds an index, or does not.
///
/// Port of `OpenMode` (`index/index_writer_config.rs`).
export enum class OpenMode : uint8_t {
    /// Create a new index; opening an existing one is an error.
    Create = 0,
    /// Append to an existing index; opening an empty directory is an error.
    Append = 1,
    /// Create the index when there is none, append otherwise.  This is the default.
    CreateOrAppend = 2,
};

/// The knobs of an [`IndexWriter`](:index_writer).
///
/// Port of `LiveIndexWriterConfig` (`index/index_writer_config.rs`), reduced to
/// the settings the port actually honours.  The reference implementation's config is a large
/// mutable object shared through an `Arc`; here it is a plain value that the
/// writer copies when it is created.
///
/// **Not modelled yet** (they belong to subsystems the port does not have):
/// deletes policy (soft deletes keep their own field), RAM accounting for the
/// `Accountable` tree (which is why flushing is driven by
/// [`max_buffered_docs`] only), commit data and event listeners.
export class IndexWriterConfig {
public:
    /// A config that indexes with `analyzer`.  The codec defaults to
    /// `Lucene101`, the similarity to BM25.
    explicit IndexWriterConfig(Analyzer &analyzer);

    IndexWriterConfig(const IndexWriterConfig &) = default;
    IndexWriterConfig &operator=(const IndexWriterConfig &) = default;
    IndexWriterConfig(IndexWriterConfig &&) noexcept = default;
    IndexWriterConfig &operator=(IndexWriterConfig &&) noexcept = default;
    ~IndexWriterConfig() = default;

    [[nodiscard]] Analyzer &analyzer() const noexcept { return *analyzer_; }

    /// The codec the writer creates segments with.
    [[nodiscard]] const Codec &codec() const noexcept { return *codec_; }
    void set_codec(std::shared_ptr<const Codec> codec);

    /// The similarity used to compute the norms at index time.
    [[nodiscard]] std::shared_ptr<const Similarity> similarity() const noexcept { return similarity_; }
    void set_similarity(std::shared_ptr<const Similarity> similarity);

    /// Flush the buffered documents once a segment holds this many.
    ///
    /// `-1` (the default) means "only flush on `commit`, `flush` or close".
    [[nodiscard]] int32_t max_buffered_docs() const noexcept { return max_buffered_docs_; }
    void set_max_buffered_docs(int32_t max_buffered_docs) noexcept { max_buffered_docs_ = max_buffered_docs; }

    /// The policy that decides which segments are merged.
    ///
    /// The default is `TieredMergePolicy`, like the reference implementation.
    [[nodiscard]] const std::shared_ptr<MergePolicy> &merge_policy() const noexcept { return merge_policy_; }
    void set_merge_policy(std::shared_ptr<MergePolicy> merge_policy);

    /// What [`IndexWriter::open`](lucene_core:index_writer) does with an existing
    /// index; [`OpenMode::CreateOrAppend`] by default.
    [[nodiscard]] OpenMode open_mode() const noexcept { return open_mode_; }
    void set_open_mode(OpenMode mode) noexcept { open_mode_ = mode; }

    /// Whether [`IndexWriter::close`](lucene_core:index_writer) commits.
    ///
    /// Port of `IndexWriterConfig::commit_on_close`: `true` (the default) commits
    /// the buffered changes, `false` drops them, which is what a rollback does.
    [[nodiscard]] bool commit_on_close() const noexcept { return commit_on_close_; }
    void set_commit_on_close(bool value) noexcept { commit_on_close_ = value; }

    /// The policy that decides when the buffered documents become a segment.
    ///
    /// Defaults to [`FlushByRamOrCountsPolicy`](lucene_core:flush_policy), which
    /// flushes on [`max_buffered_docs`] or [`ram_buffer_size_mb`].
    [[nodiscard]] const std::shared_ptr<const FlushPolicy> &flush_policy() const noexcept { return flush_policy_; }
    /// Replaces the flush policy; a null policy restores the default.
    void set_flush_policy(std::shared_ptr<const FlushPolicy> flush_policy) noexcept;

    /// The scheduler that executes the merges the policy asks for.
    ///
    /// **Deviation:** the default is `SerialMergeScheduler`, because the writer
    /// is single threaded; the reference implementation defaults to
    /// `ConcurrentMergeScheduler`.
    [[nodiscard]] const std::shared_ptr<MergeScheduler> &merge_scheduler() const noexcept { return merge_scheduler_; }
    void set_merge_scheduler(std::shared_ptr<MergeScheduler> merge_scheduler);

    /// Whether a newly written segment is packed into a compound file.
    ///
    /// Port of `IndexWriterConfig::use_compound_file` (default `true`, like the
    /// reference implementation): the segment's files are packed into a single
    /// `.cfs`/`.cfe` pair, which keeps the number of open files low once an index
    /// has many segments.  A merge additionally consults
    /// [`MergePolicy::use_compound_file`](:merge_policy), which keeps large
    /// merges unpacked (the reference implementation's "no compound file for a
    /// merge that is 10% of the index or more").
    [[nodiscard]] bool use_compound_file() const noexcept { return use_compound_file_; }
    void set_use_compound_file(bool value) noexcept { use_compound_file_ = value; }

    /// The policy that decides which commit points survive a commit.
    ///
    /// Port of `IndexWriterConfig::get_index_deletion_policy`.  The default is
    /// [`KeepOnlyLastCommitDeletionPolicy`], like the reference implementation: a
    /// commit drops the ones before it.  Replacing it with a
    /// [`NoDeletionPolicy`](:index_deletion_policy) keeps every commit point, and
    /// a [`SnapshotDeletionPolicy`](:snapshot_deletion_policy) lets a caller pin
    /// one.
    ///
    /// The writer hands the policy to its [`IndexFileDeleter`](:index_file_deleter)
    /// when it opens (which is where `on_init` runs) and runs `on_commit` after
    /// every commit.
    [[nodiscard]] const std::shared_ptr<IndexDeletionPolicy> &index_deletion_policy() const noexcept {
        return index_deletion_policy_;
    }
    /// Replaces the deletion policy; a null pointer restores the default.
    void set_index_deletion_policy(std::shared_ptr<IndexDeletionPolicy> policy);

    /// The listener told about the merges of a full flush (never null).
    [[nodiscard]] const std::shared_ptr<IndexWriterEventListener> &event_listener() const noexcept {
        return event_listener_;
    }
    /// Replaces the listener; a null pointer restores the no-op one.
    void set_event_listener(std::shared_ptr<IndexWriterEventListener> listener);

    /// The warmer run on a merged segment before it becomes visible, or null.
    [[nodiscard]] const std::shared_ptr<IndexReaderWarmer> &merged_segment_warmer() const noexcept {
        return merged_segment_warmer_;
    }
    void set_merged_segment_warmer(std::shared_ptr<IndexReaderWarmer> warmer);

    /// The order the documents of a segment are written in.
    ///
    /// **Deviation:** the reference implementation sorts the buffered documents
    /// once, while the segment is being written.  The port writes the segment
    /// unsorted into a scratch directory and then sorts it into the real
    /// directory (see [`rewrite_sorted_segment`](:sorting_rewriter)), so a sorted
    /// flush costs two writes.  The result is the same.
    /// Bytes of document data the writer buffers before it writes a segment.
    ///
    /// Port of `IndexWriterConfig::ram_buffer_size_mb` (default 16 MB, like
    /// Lucene).  **Deviation:** Lucene measures the buffers the indexing chain
    /// allocates (postings, term ids, norms, ...); the port measures the document
    /// data that goes into them (field names plus values, a few bytes of overhead
    /// per field and per document), because the in-memory writers do not report
    /// their own size.  The number is therefore a lower bound that stays
    /// proportional to the real one.
    /// How many indexing buffers the writer may use at the same time: one is being
    /// filled, the others are being written out by the threads that filled them.
    ///
    /// This is what lets several threads index at once; the RAM budget of
    /// [`ram_buffer_size_mb`] is shared between them, and a thread that finds no
    /// free buffer stalls until one comes back.
    ///
    /// Defaults to `1`, i.e. a single buffer: the port's flush thresholds are then
    /// exactly "one segment per configured buffer", which is what the rest of the
    /// API and its tests describe.  Raise it to let several threads write segments
    /// at the same time; the RAM and document-count limits are then shared between
    /// the buffers.
    /// How many threads write the full indexing buffers out, or `0` (the default) to
    /// write them on the thread that filled them.
    ///
    /// Port of the flush thread the reference implementation runs on top of its
    /// `DocumentsWriterFlushQueue`: with a worker, indexing never waits for the file
    /// I/O of a segment.  A worker only gets buffers when
    /// [`max_indexing_buffers`] is at least `2`, because a single-buffered writer has
    /// nothing to hand over.
    [[nodiscard]] size_t flush_thread_count() const noexcept { return flush_thread_count_; }
    void set_flush_thread_count(size_t count) noexcept { flush_thread_count_ = count; }

    /// A factory for the analyzer of one indexing buffer, or `nullptr`.
    ///
    /// [`analyzer`] is **not** thread safe: its token streams are reused, so two
    /// threads that tokenize at the same time corrupt each other.  A writer with
    /// more than one indexing buffer therefore either serialises the analysis on the
    /// shared analyzer, or — when this factory is set — gives every buffer its own
    /// analyzer and lets the analysis run in parallel too.
    [[nodiscard]] const std::function<std::shared_ptr<Analyzer>()> &analyzer_provider() const noexcept {
        return analyzer_provider_;
    }
    void set_analyzer_provider(std::function<std::shared_ptr<Analyzer>()> provider) {
        analyzer_provider_ = std::move(provider);
    }

    [[nodiscard]] size_t max_indexing_buffers() const noexcept { return max_indexing_buffers_; }
    void set_max_indexing_buffers(size_t count) noexcept {
        max_indexing_buffers_ = count == 0 ? 1 : count;
    }
    [[nodiscard]] double ram_buffer_size_mb() const noexcept { return ram_buffer_size_mb_; }
    void set_ram_buffer_size_mb(double megabytes) noexcept { ram_buffer_size_mb_ = megabytes; }
    /// [`ram_buffer_size_mb`] in bytes.
    [[nodiscard]] int64_t ram_buffer_size_bytes() const noexcept;

    /// The doc values field a soft delete writes its generation into, if any.
    ///
    /// Port of `IndexWriterConfig::soft_deletes_field`.  When set,
    /// [`IndexWriter::soft_update_document`] keeps the previous version of a
    /// document as a soft delete instead of dropping it without a trace.
    [[nodiscard]] const std::optional<std::string> &soft_deletes_field() const noexcept {
        return soft_deletes_field_;
    }
    void set_soft_deletes_field(std::string field) noexcept { soft_deletes_field_ = std::move(field); }
    void clear_soft_deletes_field() noexcept { soft_deletes_field_.reset(); }

    /// The query whose documents survive a soft delete.
    ///
    /// Port of the retention query of `SoftDeletesRetentionMergePolicy`
    /// (`index/soft_deletes_retention_merge_policy.rs`): a soft-deleted document
    /// that matches it is *retained* — it stays live and keeps its data instead of
    /// being hidden, which is how a version chain or a time window is preserved.
    /// A null query (the default) hides every soft-deleted document.
    ///
    /// **Deviation:** the reference implementation applies the query after a merge
    /// (`SoftDeletesRetentionOneMerge`); the port applies it while the delete is
    /// recorded, which reaches the same state without a merge: the retained
    /// document is never hidden and keeps the value of the soft deletes field it
    /// was indexed with.
    [[nodiscard]] const std::shared_ptr<const Query> &soft_deletes_retention_query() const noexcept {
        return soft_deletes_retention_query_;
    }
    /// Replaces the retention query; a null query hides every soft delete.
    void set_soft_deletes_retention_query(std::shared_ptr<const Query> query) noexcept {
        soft_deletes_retention_query_ = std::move(query);
    }

    [[nodiscard]] const std::optional<Sort> &index_sort() const noexcept { return index_sort_; }
    void set_index_sort(Sort sort) noexcept { index_sort_ = std::move(sort); }
    void clear_index_sort() noexcept { index_sort_.reset(); }

    /// Number of index-created versions recorded in the segments.
    ///
    /// Kept so a caller can pin the "created version" of the index; the port
    /// defaults to the version this build writes.
    [[nodiscard]] int32_t index_created_version_major() const noexcept {
        return index_created_version_major_;
    }
    void set_index_created_version_major(int32_t major) noexcept { index_created_version_major_ = major; }

private:
    Analyzer *analyzer_{nullptr};
    std::shared_ptr<const Codec> codec_;
    std::shared_ptr<const Similarity> similarity_;
    std::shared_ptr<MergePolicy> merge_policy_;
    std::shared_ptr<MergeScheduler> merge_scheduler_;
    std::shared_ptr<IndexWriterEventListener> event_listener_{std::make_shared<NoOpIndexWriterEventListener>()};
    std::shared_ptr<IndexReaderWarmer> merged_segment_warmer_;
    std::optional<Sort> index_sort_;
    std::optional<std::string> soft_deletes_field_;
    std::shared_ptr<const Query> soft_deletes_retention_query_;
    OpenMode open_mode_{OpenMode::CreateOrAppend};
    bool commit_on_close_{true};
    bool use_compound_file_{true};
    std::shared_ptr<IndexDeletionPolicy> index_deletion_policy_;  // set by the ctor / setter
    std::shared_ptr<const FlushPolicy> flush_policy_{FlushByRamOrCountsPolicy::create()};
    double ram_buffer_size_mb_{16.0};
    size_t max_indexing_buffers_{1};
    size_t flush_thread_count_{0};
    std::function<std::shared_ptr<Analyzer>()> analyzer_provider_;
    int32_t max_buffered_docs_{-1};
    int32_t index_created_version_major_{0};
};

} // namespace lucene
