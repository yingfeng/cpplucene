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

export module lucene_core:stored_fields;

import :types;
import std;

import :document;
import :error;
import :stored_field_visitor;

namespace lucene {

/// Loads the stored fields of a document.
///
/// Port of the `StoredFields` trait (`index/stored_fields.rs`).  Readers are
/// stateful cursors, they keep the decompressed chunk they last served, which is
/// why every method that reads takes a mutable reference.
///
/// **Deviation:** `RawStoredFieldsReader` is not part of this interface — it
/// exists to let the merge path reach the concrete reader, and merging is not
/// ported yet.
export class StoredFields {
public:
    virtual ~StoredFields() = default;

    /// Asks the reader to fetch the chunk that holds `doc_id` from storage.
    [[nodiscard]] virtual VoidResult prefetch(int32_t doc_id) = 0;

    /// Loads the fields of `doc_id` that `visitor` asks for.
    [[nodiscard]] virtual VoidResult document_with_visitor(int32_t doc_id, StoredFieldVisitor &visitor) = 0;

    /// Loads the whole document.
    [[nodiscard]] virtual Result<Document> document(int32_t doc_id) = 0;
};

} // namespace lucene
