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
import :error;
import :index_writer_config;
import :segment_commit_info;
import :term;

namespace lucene {

/// Owns the indexing buffer of the writer: the current
/// [`DocumentsWriterPerThread`](:documents_writer_per_thread), the segments it
/// already flushed, and the deletes that are waiting for the next commit.
///
/// Port of `DocumentsWriter` (`index/documents_writer.rs`, 999 lines), reduced
/// to the single-threaded case.
///
/// **Deviations** (scope):
///  * there is no `DocumentsWriterPerThreadPool` and no `FlushControl`: one
///    buffer is active at a time, and it is flushed synchronously when it
///    reaches [`IndexWriterConfig::max_buffered_docs`] (RAM accounting is not
///    ported, so the byte based flush trigger does not exist);
///  * deletes are buffered here as terms and applied by
///    [`IndexWriter`](:index_writer) at commit time (the reference implementation keeps a
///    `DocumentsWriterDeleteQueue` of nodes, each DWPT holding a slice);
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
    [[nodiscard]] VoidResult add_document(const Document &document);

    /// Buffers a delete by term.
    ///
    /// `doc_upto` is the number of documents the index held when the delete was
    /// issued: only documents before that ordinal are deleted, which is what
    /// makes `update_document` keep the document it just added.
    [[nodiscard]] VoidResult delete_documents(const Term &term, int64_t doc_upto);

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
    [[nodiscard]] const std::vector<std::pair<Term, int64_t>> &buffered_delete_terms() const noexcept;
    [[nodiscard]] bool delete_all_pending() const noexcept;
    /// Drops the buffered deletes, after a commit applied them.
    void clear_delete_buffer() noexcept;

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
