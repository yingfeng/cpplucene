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

module lucene_core:field_updates_buffer.impl;

import :field_updates_buffer;

import :bytes_ref;
import :doc_values_field_updates;
import :error;
import :index_types;
import :term;
import :types;

import std;

namespace lucene {

void FieldUpdatesBuffer::add_numeric(const Term &term, std::string field, int64_t doc_id_upto,
                                     std::optional<int64_t> value) {
    entries_.push_back(Entry{std::move(field), term, doc_id_upto, DocValuesType::Numeric, value, std::nullopt});
}

void FieldUpdatesBuffer::add_binary(const Term &term, std::string field, int64_t doc_id_upto,
                                    std::optional<BytesRef> value) {
    entries_.push_back(Entry{std::move(field), term, doc_id_upto, DocValuesType::Binary, std::nullopt,
                             std::move(value)});
}

std::set<std::string> FieldUpdatesBuffer::fields() const {
    std::set<std::string> names;
    for (const Entry &entry : entries_) {
        names.insert(entry.field);
    }
    return names;
}

DocValuesType FieldUpdatesBuffer::type_of(std::string_view field) const {
    for (const Entry &entry : entries_) {
        if (entry.field == field) {
            return entry.doc_values_type;
        }
    }
    return DocValuesType::None;
}

int64_t FieldUpdatesBuffer::ram_bytes_used() const {
    int64_t bytes = 0;
    for (const Entry &entry : entries_) {
        bytes += static_cast<int64_t>(sizeof(Entry));
        bytes += static_cast<int64_t>(entry.field.size() + entry.term.field().size() + entry.term.bytes().length());
        if (entry.binary_value.has_value()) {
            bytes += static_cast<int64_t>(entry.binary_value->length());
        }
    }
    return bytes;
}

Result<std::unique_ptr<DocValuesFieldUpdates>> resolve_field_updates(
        const FieldUpdatesBuffer &buffer, const std::string &field,
        const std::function<Result<std::vector<int32_t>>(const Term &)> &find_docs, int64_t doc_base) {
    const DocValuesType type = buffer.type_of(field);
    if (type == DocValuesType::None) {
        return err(LuceneError::illegal_state("the field \"" + field + "\" has no buffered doc values updates"));
    }

    std::unique_ptr<DocValuesFieldUpdates> updates;
    if (type == DocValuesType::Numeric) {
        updates = std::make_unique<NumericDocValuesFieldUpdates>(field, DocValuesType::Numeric);
    } else if (type == DocValuesType::Binary) {
        updates = std::make_unique<BinaryDocValuesFieldUpdates>(field, DocValuesType::Binary);
    } else {
        return err(LuceneError::illegal_argument("doc values type " + std::to_string(static_cast<int32_t>(type)) +
                                                " of field \"" + field + "\" cannot be updated in place"));
    }

    for (const FieldUpdatesBuffer::Entry &entry : buffer.entries()) {
        if (entry.field != field) {
            continue;
        }
        auto docs = find_docs(entry.term);
        if (!docs.has_value()) {
            return std::unexpected(std::move(docs.error()));
        }
        for (int32_t doc : docs.value()) {
            if (doc_base + doc >= entry.doc_id_upto) {
                // The document was indexed after the update was buffered.
                continue;
            }
            if (type == DocValuesType::Numeric) {
                if (auto added = updates->add_numeric(doc, entry.numeric_value); !added.has_value()) {
                    return std::unexpected(std::move(added.error()));
                }
            } else {
                if (auto added = updates->add_binary(doc, entry.binary_value); !added.has_value()) {
                    return std::unexpected(std::move(added.error()));
                }
            }
        }
    }

    updates->finish();
    return updates;
}

} // namespace lucene
