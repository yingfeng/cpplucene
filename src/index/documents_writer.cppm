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

export module lucene_core:documents_writer;

import :types;
import std;

import :directory;
import :document;
import :documents_writer_per_thread;
import :error;
import :index_writer_config;
import :segment_commit_info;
import :term;

namespace lucene {

/// Owns the indexing buffer of the writer: the current
/// [`DocumentsWriterPerThread`](:documents_writer_per_thread), the segments it
/// already flushed, and the deletes that are waiting for the next commit.
///
/// Port of `DocumentsWriter` (`index/documents_writer.rs`) plus the pool and the
/// flush control of `DocumentsWriterFlushControl`.
///
/// The pool holds up to [`max_buffers`] buffers: one active, the others being
/// written out by the threads that filled them.  A thread that finds no free
/// buffer stalls (that is the writer's only backpressure) instead of growing the
/// pool, so the RAM the writer uses stays bounded by
/// [`IndexWriterConfig::ram_buffer_size_mb`](lucene_core:index_writer_config)
/// whatever the number of indexing threads.
///
/// **Deviations** (scope):
///  * there is no separate flush thread and no `documents_writer_delete_queue`: a
///    buffer is active at a time, and the configured
///    [`FlushPolicy`](lucene_core:flush_policy) decides when it is written out
///    ([`FlushByRamOrCountsPolicy`](lucene_core:flush_policy) by default, which
///    compares [`IndexWriterConfig::max_buffered_docs`] and the buffered bytes
///    against [`IndexWriterConfig::ram_buffer_size_mb`]); the buffered bytes are a
///    per-document estimate of the document data, where the reference walks the
///    per-thread byte accounting of the indexing chain;
///  * deletes are buffered here as terms and applied by
///    [`IndexWriter`](:index_writer) at commit time (the reference implementation keeps a
///    `DocumentsWriterDeleteQueue` of nodes, each DWPT holding a slice);
///  * the flush queue is a plain list of flushed segments
///    ([`pending_flush_count`]) instead of a queue of tickets with a generation:
///    the writer publishes them at its next commit, near-real-time read or explicit
///    flush, which is when the reference implementation publishes its tickets too,
///    but without the split between publishing (applying the deletes) and
///    committing (writing the commit point);
///  * no merge policy and no flush notifications.
export class DocumentsWriter {
public:
    DocumentsWriter(Directory &directory, const IndexWriterConfig &config, int64_t next_segment_counter);

    DocumentsWriter(const DocumentsWriter &) = delete;
    DocumentsWriter &operator=(const DocumentsWriter &) = delete;
    DocumentsWriter(DocumentsWriter &&) noexcept = delete;
    DocumentsWriter &operator=(DocumentsWriter &&) noexcept = delete;
    ~DocumentsWriter();

    /// Indexes `document`, flushing the buffer first when it is full.
    ///
    /// **Deviation:** the reference implementation never writes a segment on the
    /// indexing thread (a flush thread does); this shorthand does, which is what a
    /// caller that wants one thread to stay simple needs.
    [[nodiscard]] VoidResult add_document(const Document &document);

    /// Buffers `document` and answers the full buffer that has to be written out,
    /// or `nullptr` when the document simply fit in the active buffer.
    ///
    /// The caller writes the buffer out **without holding the writer lock** (call
    /// [`DocumentsWriterPerThread::flush`](lucene_core:documents_writer_per_thread))
    /// and hands the result back through [`publish_flushed`].  While one thread
    /// writes, the others keep filling the pool, which is what makes indexing
    /// parallel; [`max_buffers`] caps how much RAM the pool may use, and a thread
    /// that finds no free buffer **stalls** instead of growing it.
    ///
    /// Port of the pool + flush-control part of `DocumentsWriterFlushControl`
    /// (`index/documents_writer_flush_control.rs`).
    [[nodiscard]] Result<std::unique_ptr<DocumentsWriterPerThread>> add_document_async(const Document &document);

    /// Takes back a buffer handed out by [`add_document_async`] and queues the
    /// segment it produced (`nullopt` when the buffer was empty).
    [[nodiscard]] VoidResult publish_flushed(std::optional<SegmentCommitInfo> segment, int32_t flushed_docs);

    /// Waits until every buffer a caller is writing has come back, then reports how
    /// many were outstanding.
    [[nodiscard]] VoidResult wait_for_in_flight_flushes();

    /// How many buffers the pool may hold, i.e. how many segments can be written at
    /// the same time.  `1` makes the writer flush inline, like a single-buffered
    /// writer does.
    void set_max_buffers(size_t count) noexcept;
    [[nodiscard]] size_t max_buffers() const noexcept;

    /// Where a flush worker sends the buffer it wrote.
    ///
    /// The writer installs a sink that packs the segment and queues it; without a
    /// sink the buffers are written by whoever filled them.
    using FlushSink = std::function<VoidResult(std::unique_ptr<DocumentsWriterPerThread>)>;

    /// Sets the sink the flush workers call, and how many workers there are.
    ///
    /// `count == 0` (the default) writes every buffer on the thread that filled it.
    /// A worker takes the buffers other threads hand over, so indexing never waits
    /// for the file I/O — this is what the reference implementation's flush thread
    /// does with its `DocumentsWriterFlushQueue`.
    void start_flush_threads(FlushSink sink, size_t count);

    /// Stops the flush workers, after waiting for the buffers they are writing.
    [[nodiscard]] VoidResult stop_flush_threads();

    /// Hands `buffer` to a flush worker; answers false when no worker is running.
    ///
    /// The buffer is only moved when it really goes to a worker, so the caller can
    /// still write it itself when this answers false.
    [[nodiscard]] bool enqueue_flush(std::unique_ptr<DocumentsWriterPerThread> &buffer);

    /// How many flush workers are running.
    [[nodiscard]] size_t flush_thread_count() const noexcept;

    /// Allocates the number the next segment gets; safe to call from any thread.
    [[nodiscard]] int64_t allocate_segment_counter() noexcept;

    /// Threads waiting for a free buffer right now, i.e. the stalls in progress.
    [[nodiscard]] size_t stalled_writers() const noexcept;

    /// Buffers a delete by term.
    ///
    /// `doc_upto` is the number of documents the index held when the delete was
    /// issued: only documents before that ordinal are deleted, which is what
    /// makes `update_document` keep the document it just added.
    [[nodiscard]] VoidResult delete_documents(const Term &term, int64_t doc_upto);

    /// Buffers a *soft* delete: the documents are dropped like a hard delete when
    /// the index is committed, but reported as soft deletions.
    [[nodiscard]] VoidResult soft_delete_documents(const Term &term, int64_t doc_upto);

    /// Buffers "delete every document of the index".
    [[nodiscard]] VoidResult delete_all();

    /// Flushes the buffer and returns the segments written since the last call.
    [[nodiscard]] Result<std::vector<SegmentCommitInfo>> flush_all();

    /// Segments that were flushed by [`add_document`] because the buffer filled
    /// up, since the last call.
    [[nodiscard]] std::vector<SegmentCommitInfo> take_flushed_segments();

    /// Documents buffered but not written yet.
    [[nodiscard]] int32_t pending_doc_count() const noexcept;

    /// Buffered deletes, each with the document ordinal it applies up to.
    /// One buffered delete.
    struct BufferedDelete {
        /// The term selecting the documents.
        Term term;
        /// Number of documents the index held when the delete was issued.
        int64_t doc_upto{0};
        /// Whether the delete is a soft delete.
        bool soft{false};
    };

    [[nodiscard]] const std::vector<BufferedDelete> &buffered_deletes() const noexcept;

    /// The deletes that wait for the next flush or commit.
    ///
    /// Port of `DocumentsWriterDeleteQueue` (`index/documents_writer_delete_queue.rs`),
    /// reduced to what this port needs: the entries plus the sequence number they
    /// were added with.
    ///
    /// **Deviation:** the reference implementation applies the deletes of a queue to
    /// every document writer that is still in RAM (a "tiered delete"), so a segment
    /// is written already missing them.  Here the deletes are applied when the
    /// index is committed, through the per-segment `PendingDeletes` of the reader
    /// pool — that needs the document base the commit computes, so it cannot move
    /// into the flush without duplicating that bookkeeping.  The resulting index is
    /// the same; only the wasted space of a document that a flush wrote before it
    /// was deleted is different.
    class DeleteQueue {
    public:
        /// Adds a delete and answers the sequence number it got.
        int64_t add(BufferedDelete entry);

        /// Whether any delete is waiting.
        [[nodiscard]] bool any() const noexcept { return !entries_.empty(); }
        /// How many deletes are waiting.
        [[nodiscard]] size_t size() const noexcept { return entries_.size(); }
        /// The waiting deletes, oldest first.
        [[nodiscard]] const std::vector<BufferedDelete> &entries() const noexcept { return entries_; }
        /// The sequence number the last delete got (`0` when none was added).
        [[nodiscard]] int64_t max_seq_no() const noexcept { return max_seq_no_; }
        /// Drops every waiting delete.
        void clear() noexcept {
            entries_.clear();
            max_seq_no_ = 0;
        }

    private:
        std::vector<BufferedDelete> entries_;
        int64_t max_seq_no_{0};
    };

    /// The deletes this writer buffers.
    [[nodiscard]] const DeleteQueue &delete_queue() const noexcept;
    [[nodiscard]] bool delete_all_pending() const noexcept;
    /// Drops the buffered deletes, after a commit applied them.
    void clear_delete_buffer() noexcept;

    /// Bytes of document data the active buffer holds; see
    /// [`IndexWriterConfig::ram_buffer_size_mb`].
    [[nodiscard]] int64_t bytes_used_in_ram() const noexcept;

    /// How many times the buffered documents were written out into a segment.
    [[nodiscard]] int64_t flush_count() const noexcept;

    /// How many segments are flushed but not published yet.
    ///
    /// Port of `DocumentsWriterFlushQueue::count`
    /// (`index/documents_writer_flush_queue.rs`).  The port's queue is the list of
    /// flushed segments the writer takes when it commits, so this counts the same
    /// thing the reference implementation counts as tickets.
    [[nodiscard]] size_t pending_flush_count() const noexcept;
    /// Whether a flushed segment is waiting to be published.
    [[nodiscard]] bool has_pending_flushes() const noexcept;
    /// The documents the queued segments hold; they are all live, because deletes
    /// are applied when the queue is published.
    [[nodiscard]] int64_t pending_flushed_doc_count() const noexcept;

    /// Number the next segment gets (`_<counter>`).
    [[nodiscard]] int64_t next_segment_counter() const noexcept;
    /// Sets the counter, so a caller can resume naming where it stopped.
    void set_next_segment_counter(int64_t counter) noexcept;

    /// Aborts the buffer and deletes the files of the segments it flushed.
    [[nodiscard]] VoidResult rollback();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lucene
