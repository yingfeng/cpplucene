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

export module lucene_core:index_reader;

import :types;
import std;

import :close;
import :error;
import :identity;
import :leaf_reader_context;
import :stored_fields;
import :term;
import :term_vectors;

namespace lucene {

/// A point-in-time view of an index.
///
/// Port of the `IndexReader` trait (`core/index/index_reader.rs`).
///
/// Two kinds of readers implement it: [`LeafReader`], which is atomic, and
/// composite readers (`DirectoryReader`), which only delegate document-level
/// operations to their leaves.  Searching goes through the leaves a reader
/// exposes via [`leaves`].
///
/// **Deviations** (C++ mechanics only):
///  * ref counting is not ported: ownership is expressed with `std::shared_ptr`,
///    so `inc_ref` / `dec_ref` / `ensure_open` / the parent-reader registry have
///    no counterpart and `close` is the inherited [`CloseableRef::close`];
///  * the `CacheHelper` protocol (`get_reader_cache_helper`,
///    `get_core_cache_helper`, closed listeners) is not ported yet — no cache in
///    this port consumes it;
///  * `IndexReaderContext` collapses into [`leaves`] plus each leaf context's
///    [`TopParentMeta`]: this port has no use for the parent/child context
///    tree, only for "which leaves, which doc bases, which top-level identity".
export class IndexReader : public CloseableRef {
public:
    ~IndexReader() override = default;

    /// One greater than the largest possible document number.
    [[nodiscard]] virtual Result<int32_t> max_doc() const = 0;

    /// Number of (non-deleted) documents in this index.
    [[nodiscard]] virtual Result<int32_t> num_docs() const = 0;

    /// Number of deleted documents; `max_doc - num_docs` by default.
    [[nodiscard]] virtual Result<int32_t> num_deleted_docs() const;

    /// Whether any document has been deleted.
    [[nodiscard]] virtual Result<bool> has_deletions() const;

    /// A reader for the stored fields of this index; never null, even when no
    /// stored fields were indexed.  The cursor must not be shared between
    /// threads.
    [[nodiscard]] virtual Result<std::unique_ptr<StoredFields>> stored_fields() = 0;

    /// A reader for the term vectors of this index; never null, even when no
    /// term vectors were indexed.  The cursor must not be shared between
    /// threads.
    [[nodiscard]] virtual Result<std::unique_ptr<TermVectors>> term_vectors() = 0;

    /// Number of documents containing `term`; `0` when the term or field does
    /// not exist.  Deleted documents are *not* taken into account.
    [[nodiscard]] virtual Result<int32_t> doc_freq(const Term &term) = 0;

    /// Number of occurrences of `term` across all documents.  Deleted documents
    /// are not taken into account.
    [[nodiscard]] virtual Result<int64_t> total_term_freq(const Term &term) = 0;

    /// Sum of the document frequencies of all terms of `field`.
    [[nodiscard]] virtual Result<int64_t> sum_doc_freq(std::string_view field) = 0;

    /// Number of documents that have at least one term for `field`.
    [[nodiscard]] virtual Result<int32_t> doc_count(std::string_view field) = 0;

    /// Sum of the total term frequencies of all terms of `field`.
    [[nodiscard]] virtual Result<int64_t> sum_total_term_freq(std::string_view field) = 0;

    /// The atomic leaves of this reader, in document order: `leaves()[i]` serves
    /// the documents `[doc_base, doc_base + max_doc)`.  A leaf reader returns
    /// itself.  The span stays valid for the lifetime of the reader.
    [[nodiscard]] virtual Result<std::span<const LeafReaderContext>> leaves() const = 0;

    /// Identity of the top-level reader this reader belongs to; copied into the
    /// [`TopParentMeta`] of every leaf context.
    [[nodiscard]] virtual const Identity &identity() const = 0;
};

} // namespace lucene
