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

export module lucene_core:documents_writer_per_thread;

import :types;
import std;

import :analyzer;
import :codec;
import :directory;
import :document;
import :error;
import :indexing_chain;
import :segment_commit_info;
import :segment_info;
import :similarity;

namespace lucene {

/// The indexing buffer of one segment.
///
/// Port of `DocumentsWriterPerThread` (`index/documents_writer_per_thread.rs`,
/// 1161 lines), reduced to the single-threaded write path.
///
/// A DWPT owns one [`IndexingChain`], the [`SegmentInfo`] it is filling and the
/// [`FieldInfosBuilder`] of the segment.  Documents are added with
/// [`add_document`]; [`flush`] turns the buffered documents into segment files
/// and returns the [`SegmentCommitInfo`] that references them.  After a flush
/// the instance is spent (a new segment means a new chain and a new segment
/// info), which is what the reference implementation's `DocumentsWriterPerThreadPool` recycles.
///
/// **Deviations** (scope):
///  * no delete queue / delete slices: deletes are buffered by
///    [`DocumentsWriter`](:documents_writer) and applied when the index is
///    committed, so a DWPT never needs to resolve a term;
///  * no index sorting, no `pending_num_docs` accounting and no
///    `TrackingDirectoryWrapper`: the segment files are collected from the
///    directory after the codecs wrote them;
///  * non-aborting failures do not abort the writer.  The document keeps its id
///    and is recorded as deleted, so the segment stays consistent
///    (`delete_last_docs` in the reference implementation).
export class DocumentsWriterPerThread {
public:
    DocumentsWriterPerThread(Directory &directory, const Codec &codec, SegmentInfo segment_info,
                             int32_t index_created_version_major, Analyzer &analyzer,
                             std::shared_ptr<const Similarity> similarity);

    DocumentsWriterPerThread(const DocumentsWriterPerThread &) = delete;
    DocumentsWriterPerThread &operator=(const DocumentsWriterPerThread &) = delete;
    DocumentsWriterPerThread(DocumentsWriterPerThread &&) noexcept = delete;
    DocumentsWriterPerThread &operator=(DocumentsWriterPerThread &&) noexcept = delete;
    ~DocumentsWriterPerThread();

    /// Indexes `document` as the next document of this segment.
    ///
    /// On failure the document keeps its id and is marked deleted, so the
    /// segment keeps a hole instead of losing the buffered documents that came
    /// before it.
    [[nodiscard]] VoidResult add_document(const Document &document);

    /// Writes the segment files and returns the commit that references them.
    ///
    /// Returns an empty optional when nothing was buffered, and fails when the
    /// instance was already flushed or aborted.
    [[nodiscard]] Result<std::optional<SegmentCommitInfo>> flush();

    /// Drops the buffers; the instance cannot be used afterwards.
    [[nodiscard]] VoidResult abort();

    [[nodiscard]] int32_t num_docs_in_ram() const noexcept;
    /// Number of documents that were marked deleted because indexing failed.
    [[nodiscard]] int32_t num_deleted_doc_ids() const noexcept;
    [[nodiscard]] const SegmentInfo &segment_info() const noexcept;
    [[nodiscard]] bool is_aborted() const noexcept;
    [[nodiscard]] bool has_flushed() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lucene
