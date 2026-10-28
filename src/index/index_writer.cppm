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

export module lucene_core:index_writer;

import :types;
import std;

import :bytes_ref;
import :codec_reader;
import :directory;
import :doc_values_update;
import :doc_values_update;
import :document;
import :error;
import :index_reader;
import :index_writer_config;
import :index_writer_config;
import :lock;
import :query;
import :segment_infos;
import :standard_directory_reader;
import :term;

namespace lucene {

/// How many documents an index holds.
///
/// Port of `DocStats` (`index/index_writer.rs`): `num_docs` excludes the deleted
/// documents, `max_doc` counts every document that was ever added.
export struct DocStats {
    int64_t num_docs{0};
    int64_t max_doc{0};
};

/// Creates and updates an index.
///
/// Port of `IndexWriter` (`index/index_writer.rs`), the first working version.
/// It wires together everything the write path needs: a
/// [`DocumentsWriter`](:documents_writer) to buffer documents, the per-thread
/// writer and the indexing chain to turn them into segment files, and
/// [`SegmentInfos`] to commit the result so readers can open it.
///
/// Usage:
/// ```text
/// auto writer = IndexWriter::open(directory, config);
/// writer.value()->add_document(doc);
/// writer.value()->commit();
/// ```
///
/// **Deviations** (scope, see the project note):
///  * a writer-level mutex serializes the public calls, so several threads may
///    share one writer (the reference implementation locks per
///    `DocumentsWriterPerThread`, which lets them index in parallel); there is no
///    lock manager, no thread states and no concurrent flush.  The lock is held
///    while the listener callbacks run, so a listener must not call back into the
///    same writer from another thread;
///  * merges run synchronously inside [`commit`] and [`force_merge`]: there is
///    no merge scheduler and no merge thread, so a merge blocks the caller;
///  * deletes are applied at commit time by rewriting the `.liv` of every
///    segment the term matches (the reference implementation applies them incrementally to the DWPTs
///    through its delete queue), and `delete_all` drops the segments;
///  * doc values updates are buffered per `(term, field, value)` and written at
///    commit time as a new doc values generation of each affected segment (the
///    reference implementation buffers one entry per document and applies them
///    to the pending DWPTs); soft deletes, commit data, listeners and a merge
///    scheduler are absent, and `rollback` simply removes the segments written
///    since the last commit;
///  * [`get_reader`] flushes the buffered documents and applies the buffered
///    deletes before it opens its readers, and it reuses the readers of the
///    previous near-real-time read through a [`ReaderPool`](:reader_pool); a
///    segment is reopened only when its deletions, field infos or doc values
///    moved on.
export class IndexWriter {
public:
    /// Opens (or creates) the index in `directory`.
    [[nodiscard]] static Result<std::unique_ptr<IndexWriter>> open(Directory &directory,
                                                                   const IndexWriterConfig &config);

    IndexWriter(const IndexWriter &) = delete;
    IndexWriter &operator=(const IndexWriter &) = delete;
    IndexWriter(IndexWriter &&) noexcept = delete;
    IndexWriter &operator=(IndexWriter &&) noexcept = delete;
    ~IndexWriter();

    /// Adds a document; the buffer is flushed automatically when it is full.
    [[nodiscard]] VoidResult add_document(const Document &document);

    /// Adds every document of `documents`, in order.
    ///
    /// Port of `IndexWriter::add_documents`.  The documents are added one by one,
    /// so a failure leaves the ones before it in the buffer.
    [[nodiscard]] VoidResult add_documents(std::span<const Document *const> documents);

    /// Deletes every document matching `term`, then adds `document`.
    [[nodiscard]] VoidResult update_document(const Term &term, const Document &document);

    /// Deletes every document matching `term`, once the index is committed.
    [[nodiscard]] VoidResult delete_documents(const Term &term);

    /// Deletes every document matching `query`, once the index is committed.
    ///
    /// Port of `IndexWriter::delete_documents(Query...)`.  The query is buffered,
    /// so it only sees the documents that exist when it is issued.
    ///
    /// **Deviation:** the query is taken by `shared_ptr` because this port's
    /// [`Query`] has no `clone`; the writer keeps the query alive until the delete
    /// is applied, while Lucene copies it and lets the caller release it.
    [[nodiscard]] VoidResult delete_documents(std::shared_ptr<const Query> query);

    /// Deletes the documents matching any of `queries`.
    [[nodiscard]] VoidResult delete_documents(std::span<const std::shared_ptr<const Query>> queries);

    /// Buffers a delete of one document of `reader`.
    ///
    /// Port of `IndexWriter::try_delete_document`: `doc_id` is a document of
    /// `reader`, which has to be a reader over this index.  Reports `false` when
    /// the document cannot be resolved (an unknown reader), which lets the caller
    /// fall back to a term delete.
    [[nodiscard]] Result<bool> try_delete_document(const IndexReader &reader, int32_t doc_id);

    /// Copies every segment of the index in `source` into this index.
    ///
    /// Port of `IndexWriter::add_indexes_from_directory`.  The segments are copied
    /// under fresh names (every file is renamed with them and the `.si` is
    /// rewritten), then committed, so the index becomes visible as a whole.
    ///
    /// **Deviations:** a source that is this writer's own directory is rejected
    /// (as in Lucene), but a source built by an incompatible codec is not checked
    /// up front — the failure shows up when a reader opens the copied segment.
    [[nodiscard]] VoidResult add_indexes_from_directory(Directory &source);

    /// Merges `readers` into this index as a new segment and commits.
    ///
    /// Port of `IndexWriter::add_indexes_from_codec_readers`; returns the number
    /// of documents the readers contributed.  **Deviation:** the reference
    /// implementation hands the readers to the merge policy (which may produce
    /// several segments), this port merges them into one.
    [[nodiscard]] Result<int64_t> add_indexes_from_codec_readers(
            const std::vector<std::shared_ptr<CodecReader>> &readers);

    /// Deletes the files no commit the writer knows about references.
    ///
    /// Port of `IndexWriter::delete_unused_files`.  **Deviation:** the reference
    /// implementation asks its `IndexFileDeleter`, which tracks a reference count
    /// per file; the port has no such bookkeeping, so it keeps whatever
    /// [`segment_infos`] and the last commit name and drops everything else that
    /// looks like a segment file.  A reader opened from an older commit keeps its
    /// files alive through the directory, so this only removes leftovers of an
    /// interrupted writer.
    [[nodiscard]] VoidResult delete_unused_files();

    /// Replaces the documents matching `term` with `document`, as a *soft* update.
    ///
    /// Port of `IndexWriter::soft_update_document`: the previous versions are
    /// dropped when the index is committed and reported as soft deletions
    /// ([`SegmentCommitInfo::soft_del_count`]), which is what a retention policy
    /// looks at.  `document` has to carry the field named by
    /// [`IndexWriterConfig::soft_deletes_field`].
    ///
    /// **Deviation:** the reference implementation leaves the old versions in the
    /// segment and hides them when reading, so a retention merge policy can revive
    /// them.  This port materializes deletes at commit time (see the class note),
    /// so a soft-deleted document cannot come back; the API and the bookkeeping
    /// are in place, the revival is not.
    [[nodiscard]] VoidResult soft_update_document(const Term &term, const Document &document);

    /// Updates a numeric doc values field of every document matching `term`.
    ///
    /// Port of `IndexWriter::update_numeric_doc_value`.  The update is buffered
    /// and written when the index is committed, as a new doc values generation
    /// of each segment it touches.  An empty `value` removes the value, like
    /// `updateNumericDocValue(term, field, null)` does.  Only
    /// [`DocValuesType::Numeric`] and [`DocValuesType::SortedNumeric`] fields can
    /// be updated in place; anything else is an `illegal_argument`.
    [[nodiscard]] VoidResult update_numeric_doc_value(const Term &term, std::string field,
                                                     std::optional<int64_t> value);

    /// Applies every buffered [`DocValuesUpdate`] at once.
    ///
    /// Port of `IndexWriter::update_doc_values`: the same new doc values generation
    /// is written for every field the updates touch, so a caller can move several
    /// fields of the same documents in one call.
    [[nodiscard]] VoidResult update_doc_values(std::span<const DocValuesUpdate> updates);

    /// Updates the doc values of one document of `reader`.
    ///
    /// Port of `IndexWriter::try_update_doc_value`.  Reports `false` when the
    /// document cannot be resolved (an unknown reader); `doc_id` is a document of
    /// `reader`, exactly like in [`try_delete_document`].
    [[nodiscard]] Result<bool> try_update_doc_value(const IndexReader &reader, int32_t doc_id,
                                                    std::span<const DocValuesUpdate> updates);

    /// Updates a binary doc values field of every document matching `term`.
    ///
    /// Port of `IndexWriter::update_binary_doc_value`; see
    /// [`update_numeric_doc_value`].  Only [`DocValuesType::Binary`] and
    /// [`DocValuesType::SortedSet`] fields can be updated in place.
    [[nodiscard]] VoidResult update_binary_doc_value(const Term &term, std::string field,
                                                    std::optional<BytesRef> value);

    /// Deletes every document of the index.
    [[nodiscard]] VoidResult delete_all();

    /// Writes the buffered documents into new segments (no commit yet).
    [[nodiscard]] VoidResult flush();

    /// Flushes, applies the buffered deletes and writes the next `segments_N`.
    /// First phase of a two-phase commit: everything the commit does **except**
    /// making it visible.
    ///
    /// Port of `IndexWriter::prepareCommit`: the buffered documents become segments,
    /// the deletes and the doc values updates are applied and the merges run, but
    /// `segments_N` stays as it is, so a reader opened meanwhile still sees the
    /// previous commit.  Answers the generation the commit will land in.
    ///
    /// A caller that coordinates several writers calls this on all of them first and
    /// then [`commit_prepared`] on all of them; if any of the two phases fails it
    /// calls [`rollback`] on the others.  Nothing may be added between the phases.
    [[nodiscard]] Result<int64_t> prepare_commit();

    /// Second phase: publishes what [`prepare_commit`] prepared, and answers the new
    /// generation.  It does very little work (write `segments_N`, sync, run the
    /// deletion policy).
    [[nodiscard]] Result<int64_t> commit_prepared();

    /// Whether a prepared commit is waiting for its second phase.
    [[nodiscard]] bool has_prepared_commit() const;

    /// **Deviation:** the reference implementation's `IndexWriter` *implements* the
    /// [`TwoPhaseCommit`](lucene_core:two_phase_commit) interface, whose `commit()`
    /// returns the new generation.  This port's `commit()` already existed with a
    /// `VoidResult` (the generation is an implementation detail here), so the writer
    /// offers the same protocol under [`prepare_commit`] and [`commit_prepared`]
    /// instead of inheriting it; a user-defined participant still implements the
    /// interface.

    [[nodiscard]] VoidResult commit();

    /// Merges the index down to at most `max_num_segments` segments.
    ///
    /// The merge starts once the buffered documents are flushed and finishes
    /// before this call returns, like `forceMerge(1)` in the reference
    /// implementation when there is no merge scheduler.
    [[nodiscard]] VoidResult force_merge(int32_t max_num_segments);

    /// Rewrites the segments that have deletes, to expunge them.
    [[nodiscard]] VoidResult force_merge_deletes();

    /// Drops the documents added since the last commit and restores the last
    /// committed state of the index.
    [[nodiscard]] VoidResult rollback();

    /// [`commit`] followed by releasing the writer.
    [[nodiscard]] VoidResult close();

    /// A reader over the index as it is right now, without committing.
    ///
    /// Port of `IndexWriter::get_reader`, the near-real-time read: the buffered
    /// documents are flushed, the buffered deletes are applied, and the segments
    /// the writer currently holds are opened.  The result is a snapshot: writes
    /// that happen afterwards are not visible through it.
    [[nodiscard]] Result<std::shared_ptr<DirectoryReader>> get_reader();

    /// [`get_reader`](Self::get_reader) with the expert switches of the reference
    /// implementation's `open_with_writer_deletes`.
    ///
    /// `apply_all_deletes` decides whether the buffered deletes are applied (and
    /// written to the `.liv` files) before the reader opens; with `false` the
    /// reader still shows the documents a pending delete targets.  **Deviation:**
    /// `write_all_deletes` is accepted for compatibility but the port always
    /// writes the live docs when it applies deletes, because it never carries
    /// them in heap.
    [[nodiscard]] Result<std::shared_ptr<DirectoryReader>> get_reader(bool apply_all_deletes,
                                                                     bool write_all_deletes);

    /// Documents visible in the index: committed ones plus the buffered ones,
    /// minus the deletes that are still only buffered.
    [[nodiscard]] Result<int64_t> num_docs() const;

    /// Sum of the `max_doc` of every segment, buffered documents included.
    [[nodiscard]] Result<int64_t> max_doc() const;

    /// Whether a committed segment has deletions.
    [[nodiscard]] Result<bool> has_deletions() const;

    /// Deletes the documents matching `query` and adds `document`, in one step.
    ///
    /// Port of `IndexWriter::update_documents_with_query`.  The delete is buffered,
    /// so it only reaches the documents that exist now — the document added here is
    /// the new version.
    [[nodiscard]] VoidResult update_document(std::shared_ptr<const Query> query, const Document &document);

    /// Deletes every document matching `term` and adds all of `documents`.
    ///
    /// Port of `IndexWriter::update_documents_with_term` (`index/index_writer.rs`):
    /// the delete is buffered once, so the documents added here survive it.
    [[nodiscard]] VoidResult update_documents(const Term &term, std::span<const Document *const> documents);

    /// Soft-updates every document of `documents` against `term`.
    ///
    /// Port of `IndexWriter::soft_update_documents`; see
    /// [`soft_update_document`].
    [[nodiscard]] VoidResult soft_update_documents(const Term &term, std::span<const Document *const> documents);

    /// Every field name the index currently holds, sorted.
    ///
    /// Port of `IndexWriter::get_field_names`.  The names come from the `.fnm` of
    /// each segment the writer holds, so a field of a document that is still
    /// buffered is not listed.
    [[nodiscard]] Result<std::vector<std::string>> get_field_names() const;

    /// Runs the merge policy once and waits for the merges it asks for.
    ///
    /// Port of `IndexWriter::maybe_merge`; the port has no merge thread, so the
    /// merges run inside the call.
    [[nodiscard]] VoidResult maybe_merge();

    /// Whether a merge from a previous call is still queued.
    [[nodiscard]] bool has_pending_merges() const;

    /// The user data the last commit recorded.
    ///
    /// Port of `IndexWriter::get_live_commit_data`.
    [[nodiscard]] const std::map<std::string, std::string> &get_live_commit_data() const;

    /// The directory this writer writes to.
    [[nodiscard]] Directory &get_directory() const;

    /// The configuration this writer was opened with.
    [[nodiscard]] const IndexWriterConfig &get_config() const;

    /// How many times the buffered documents were written out into a segment.
    ///
    /// Port of `IndexWriter::get_flush_count`.
    [[nodiscard]] int64_t flush_count() const;

    /// How many segments are flushed but not published yet.
    ///
    /// Port of the count of `DocumentsWriterFlushQueue`
    /// (`index/documents_writer_flush_queue.rs`): a flushed segment waits in the
    /// writer's queue until the next commit (or near-real-time read) publishes it.
    [[nodiscard]] size_t pending_flush_count() const;

    /// Bytes of document data the writer currently buffers in RAM.
    ///
    /// Port of `IndexWriter::get_flushing_bytes`; see
    /// [`IndexWriterConfig::ram_buffer_size_mb`] about how the number is measured.
    [[nodiscard]] int64_t get_flushing_bytes() const;

    /// Documents the index holds, buffered ones included.
    ///
    /// Port of `IndexWriter::get_pending_num_docs`.
    [[nodiscard]] Result<int64_t> get_pending_num_docs() const;

    /// How many documents the index holds, live and deleted.
    ///
    /// Port of `IndexWriter::get_doc_stats` (`index/index_writer.rs`), which answers
    /// the reference implementation's `DocStats`: `num_docs` counts the live
    /// documents (buffered and flushed ones included), `max_doc` every document the
    /// writer ever saw.
    [[nodiscard]] DocStats get_doc_stats() const;

    /// Whether anything was added or deleted since the last commit.
    [[nodiscard]] bool has_uncommitted_changes() const;

    /// Documents buffered but not written into a segment yet.
    [[nodiscard]] int32_t num_buffered_documents() const;

    /// Number of segments the writer currently holds.
    [[nodiscard]] size_t segment_count() const;

    /// Replaces the user data the next commit records.
    ///
    /// Port of `IndexWriter::set_live_commit_data`; `segments_N` carries it, and
    /// [`IndexCommit::user_data`] hands it back after a commit.
    void set_live_commit_data(std::map<std::string, std::string> user_data);

    /// The commit state of the index as the writer currently sees it.
    [[nodiscard]] const SegmentInfos &segment_infos() const;

    [[nodiscard]] bool is_open() const noexcept;

private:
    explicit IndexWriter(Directory &directory, IndexWriterConfig config, std::unique_ptr<Lock> write_lock);

    struct Impl;
    std::unique_ptr<Impl> impl_;

    /// Waits for the merges a background scheduler started.
    ///
    /// It is the only writer operation that must run **without** the writer lock:
    /// a background merge takes that lock itself to publish, so waiting for it
    /// while holding the lock would deadlock.
    [[nodiscard]] VoidResult wait_for_merges_unlocked();
    /// Serializes the public calls.  Recursive because `update_document` and
    /// `soft_update_document` add through the public `add_document`; the reference
    /// implementation does not need this, but its per-thread writers do.
    mutable std::recursive_mutex mutex_;
};

} // namespace lucene
