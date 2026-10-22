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

export module lucene_core:indexing_chain;

import :types;
import std;

import :analyzer;
import :codec;
import :directory;
import :document;
import :error;
import :field_infos_builder;
import :indexable_field;
import :segment_info;
import :segment_write_state;
import :similarity;

namespace lucene {

/// Turns a [`Document`] into the buffers of one segment and writes them out.
///
/// Port of `IndexingChain` (`index/indexing_chain.rs`).  One instance lives as
/// long as the indexing buffer of a segment: it keeps the term dictionaries,
/// the doc values writers and the stored fields of every document added so far,
/// and [`flush`] turns all of that into the segment's files.
///
/// For each document the chain does two passes over the fields: the first
/// collects the schema of every distinct field name (so all instances of a
/// multi-valued field are handled together — an analyzer reuses its token
/// stream, so two fields cannot be inverted at the same time), the second
/// indexes them.  The norm of each indexed field is computed once, after both
/// passes, from the accumulated [`FieldInvertState`](:field_invert_state).
///
/// **Deviations** (scope, see the project note):
///  * index sorting is not ported, so `flush` needs no sort map and no doc map;
///  * point values and vector values are not indexed: a field whose type has
///    points or a vector dimension is rejected with `unsupported_operation`
///    rather than silently dropped;
///  * update/soft-delete bookkeeping belongs to `DocumentsWriterPerThread` and
///    is not part of this class;
///  * the reference implementation passes the directory, the segment info and the live writer config
///    to every method; here they are captured at construction (the C++ habit)
///    and only the field infos builder is passed in, because it grows with the
///    documents.
export class IndexingChain {
public:
    /// `index_created_version_major` is the created-version of the index, which
    /// `FieldInvertState` records so the similarity can pick a norm encoding.
    IndexingChain(Directory &directory, const Codec &codec, SegmentInfo &segment_info,
                  int32_t index_created_version_major, Analyzer &analyzer,
                  std::shared_ptr<const Similarity> similarity = nullptr);

    IndexingChain(const IndexingChain &) = delete;
    IndexingChain &operator=(const IndexingChain &) = delete;
    IndexingChain(IndexingChain &&) noexcept = delete;
    IndexingChain &operator=(IndexingChain &&) noexcept = delete;
    ~IndexingChain();

    /// Indexes `document` as document `doc_id`.
    ///
    /// `field_infos` collects the schema of the segment; a new field is added
    /// the first time it appears, and from then on every document is checked
    /// against it (an inconsistent field is an error, not a silent corruption).
    [[nodiscard]] VoidResult process_document(int32_t doc_id, const Document &document,
                                              FieldInfosBuilder &field_infos);

    /// Writes the buffers of the documents added so far.
    ///
    /// `state.field_infos` must be the finished [`FieldInfos`](:field_infos) of
    /// the segment (the caller builds it from `field_infos` once the last
    /// document was processed), because the codecs read the schema from it.
    [[nodiscard]] VoidResult flush(const SegmentWriteState &state);

    /// Drops everything buffered so far, for a segment that will not be written.
    [[nodiscard]] VoidResult abort();

    /// Number of distinct field names seen so far.
    [[nodiscard]] size_t num_per_fields() const noexcept;

    /// The generator handed to the next document (the reference implementation's `next_field_gen`).
    [[nodiscard]] int64_t next_field_gen() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lucene
