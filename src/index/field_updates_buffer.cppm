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

export module lucene_core:field_updates_buffer;

import :accountable;
import :types;
import std;

import :bytes_ref;
import :doc_values_field_updates;
import :error;
import :index_types;
import :term;

namespace lucene {

/// The doc values updates an index writer buffered since the last commit.
///
/// Port of `FieldUpdatesBuffer` (`index/field_updates_buffer.rs`).  The
/// reference implementation buffers one entry per *document*, because it applies
/// the updates to each document writer as the documents arrive.  This port
/// applies doc values updates when the index is committed (see
/// `IndexWriter::apply_doc_values_updates`), so an entry is one buffered
/// `(term, field, value)` triple that is later resolved into the documents it
/// selects — which is the same information in a tenth of the space.
export class FieldUpdatesBuffer : public Accountable {
public:
    /// One buffered update, before its documents are resolved.
    struct Entry {
        /// The field the update applies to.
        std::string field;
        /// The term selecting the documents.
        Term term;
        /// Number of documents the index held when the update was buffered.
        int64_t doc_id_upto{0};
        /// [`DocValuesType::Numeric`] or [`DocValuesType::Binary`].
        DocValuesType doc_values_type{DocValuesType::None};
        /// The new value of a numeric field; empty removes the value.
        std::optional<int64_t> numeric_value;
        /// The new value of a binary field; empty removes the value.
        std::optional<BytesRef> binary_value;
    };

    /// Buffers an update of a numeric field; an empty `value` removes it.
    void add_numeric(const Term &term, std::string field, int64_t doc_id_upto, std::optional<int64_t> value);

    /// Buffers an update of a binary field; an empty `value` removes it.
    void add_binary(const Term &term, std::string field, int64_t doc_id_upto, std::optional<BytesRef> value);

    /// Whether anything is buffered.
    [[nodiscard]] bool any() const noexcept { return !entries_.empty(); }
    /// Number of buffered entries.
    [[nodiscard]] size_t size() const noexcept { return entries_.size(); }
    /// The buffered entries, in the order they were added.
    [[nodiscard]] const std::vector<Entry> &entries() const noexcept { return entries_; }
    /// The fields with buffered updates, sorted.
    [[nodiscard]] std::set<std::string> fields() const;
    /// The doc values type buffered for `field`, or `None`.
    [[nodiscard]] DocValuesType type_of(std::string_view field) const;
    /// The bytes the buffer holds, as reported by `ramBytesUsed`.
    [[nodiscard]] int64_t ram_bytes_used() const override;

    /// Drops every entry, after they were applied.
    void clear() noexcept { entries_.clear(); }

private:
    std::vector<Entry> entries_;
};

/// Resolves the buffered updates of one field into per-document updates.
///
/// `find_docs` returns the documents of the segment that match a term, in
/// ascending order; `doc_base` is the number of documents the segments before
/// this one hold, so an update is not applied to documents indexed after it was
/// buffered.  Later entries win over earlier ones, like repeated updates in one
/// commit do in Lucene.
[[nodiscard]] Result<std::unique_ptr<DocValuesFieldUpdates>> resolve_field_updates(
        const FieldUpdatesBuffer &buffer, const std::string &field,
        const std::function<Result<std::vector<int32_t>>(const Term &)> &find_docs, int64_t doc_base);

} // namespace lucene
