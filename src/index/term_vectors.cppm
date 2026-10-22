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

export module lucene_core:term_vectors;

import :types;
import std;

import :error;
import :fields;
import :terms;

namespace lucene {

/// Reads the term vectors of a document.
///
/// Port of the `TermVectors` trait (`core/index/term_vectors.rs`).
///
/// The returned [`Fields`] behaves like a single-document inverted index: the
/// document id is `0` and every term of the document is a posting with a
/// frequency of at least one.  When offsets were indexed they are reachable
/// through `PostingsEnum::start_offset` / `end_offset`.
///
/// Readers are stateful cursors (they keep the chunk they last decoded), which
/// is why [`get`] takes no constness for granted and a reader is never shared
/// between threads.
///
/// **Deviation:** the `RawTermVectors` companion trait of the reference implementation exists to let
/// the merge path reach the concrete reader's `IndexInput`; merging is not
/// ported, so it is left out.
export class TermVectors {
public:
    virtual ~TermVectors() = default;

    /// Hints that `doc_id` will be read soon, so its chunk can be fetched from
    /// storage in advance.
    [[nodiscard]] virtual VoidResult prefetch(int32_t /*doc_id*/) { return {}; }

    /// The term vectors of `doc`, or `nullptr` when the document has none.
    [[nodiscard]] virtual Result<std::unique_ptr<Fields>> get(int32_t doc) = 0;

    /// The [`Terms`] of one field of `doc`; `nullptr` when that field has no
    /// term vector.
    ///
    /// The default implementation goes through [`get`].
    [[nodiscard]] virtual Result<std::unique_ptr<Terms>> get_field_terms(int32_t doc, std::string_view field);
};

} // namespace lucene
