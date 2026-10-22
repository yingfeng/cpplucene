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

export module lucene_core:term_vectors_writer;

import :types;
import std;

import :close;
import :data_input;
import :error;
import :field_infos;
import :fields;
import :merge_state;

namespace lucene {

/// Writes the term vectors of the documents of a segment.
///
/// Port of the `TermVectorsWriter` trait (`codecs/term_vectors_writer.rs`).
/// The call sequence per document is
/// `start_document`, then per field `start_field` / `start_term` /
/// (`add_position` | `add_prox`)*`/ `finish_field`, then `finish_document`.
/// A document without term vectors still calls `start_document(0)` and
/// `finish_document`.
export class TermVectorsWriter : public Closeable {
public:
    ~TermVectorsWriter() override = default;

    /// Starts a document with `num_vector_fields` fields that have a vector.
    [[nodiscard]] virtual VoidResult start_document(int32_t num_vector_fields) = 0;

    /// Ends the current document.
    [[nodiscard]] virtual VoidResult finish_document() = 0;

    /// Starts a field whose vector has `num_terms` terms.
    ///
    /// `positions` / `offsets` / `payloads` say which of the optional arrays
    /// the subsequent calls will provide.
    [[nodiscard]] virtual VoidResult start_field(const FieldInfo &field_info, int32_t num_terms, bool positions,
                                                 bool offsets, bool payloads) = 0;

    /// Ends the current field.
    [[nodiscard]] virtual VoidResult finish_field() = 0;

    /// Adds a term with frequency `freq` (`>= 1`); terms must arrive in
    /// lexicographic order, which is what makes prefix compression possible.
    [[nodiscard]] virtual VoidResult start_term(std::span<const uint8_t> term, int32_t freq) = 0;

    /// Adds one occurrence of the current term.
    ///
    /// `positions` / `offsets` are only stored when [`start_field`] asked for
    /// them; `payload` on `std::nullopt` means "no payload for this
    /// occurrence".
    [[nodiscard]] virtual VoidResult add_position(int32_t position, int32_t start_offset, int32_t end_offset,
                                                 std::optional<std::span<const uint8_t>> payload) = 0;

    /// Expert API: adds `num_prox` positions (and offsets) taken from the
    /// packed streams of the indexer instead of through [`add_position`].
    ///
    /// The default implementation reports `need_implemented`.
    [[nodiscard]] virtual VoidResult add_prox(size_t num_prox, DataInput *positions, DataInput *offsets);

    /// Called once, with the number of documents that were written.
    [[nodiscard]] virtual VoidResult finish(int32_t num_docs) = 0;

    /// Writes every field of one document's vectors.
    ///
    /// Port of `TermVectorsWriter::add_all_doc_vectors`: a null `vectors` still
    /// writes an empty document, and every term of every field is re-emitted
    /// through [`start_term`] / [`add_position`].  `merge_state_meta` is only
    /// needed to resolve the field infos of the merged segment.
    [[nodiscard]] virtual VoidResult add_all_doc_vectors(Fields *vectors, const MergeStateMeta &merge_state_meta);

    /// Merges the term vectors of the readers in `merge_state`, skipping deleted
    /// documents, and returns the number of documents written.
    ///
    /// Port of `TermVectorsWriter::merge`.
    [[nodiscard]] virtual Result<int32_t> merge(MergeState &merge_state);
};

} // namespace lucene
