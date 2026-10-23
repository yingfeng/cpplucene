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

export module lucene_core:sorting_field_readers;

import :types;
import std;

import :document;
import :error;
import :fields;
import :index_sorter;
import :stored_field_visitor;
import :stored_fields;
import :stored_fields_reader;
import :term_vectors;
import :term_vectors_reader;
import :terms;

namespace lucene {

/// Stored fields presented in index-sorted order.
///
/// Port of the stored fields half of `sorting_codec_reader.rs`
/// (`core/index/sorting_codec_reader.rs`).
///
/// Document `N` of this reader is document `new_to_old(N)` of the segment it
/// wraps, so a segment written in insertion order can be re-written in sorted
/// order without touching the other readers.
export class SortingStoredFieldsReader final : public StoredFieldsReader {
public:
    /// Wraps `reader`; both arguments must outlive the wrapper.
    [[nodiscard]] static Result<std::unique_ptr<SortingStoredFieldsReader>> create(
            std::shared_ptr<StoredFieldsReader> reader, std::shared_ptr<const IndexSortDocMap> doc_map);

    // ---- StoredFields ----

    [[nodiscard]] VoidResult prefetch(int32_t doc_id) override;
    [[nodiscard]] VoidResult document_with_visitor(int32_t doc_id, StoredFieldVisitor &visitor) override;
    [[nodiscard]] Result<Document> document(int32_t doc_id) override;

    // ---- StoredFieldsReader ----

    [[nodiscard]] VoidResult check_integrity() const override;
    [[nodiscard]] Result<std::unique_ptr<StoredFieldsReader>> get_merge_instance() const override;
    [[nodiscard]] Result<std::unique_ptr<StoredFieldsReader>> clone() const override;
    [[nodiscard]] VoidResult close() const override;

private:
    SortingStoredFieldsReader(std::shared_ptr<StoredFieldsReader> reader, std::shared_ptr<const IndexSortDocMap> doc_map);

    /// The document of the wrapped reader that `doc_id` stands for.
    [[nodiscard]] Result<int32_t> old_doc(int32_t doc_id) const;

    std::shared_ptr<StoredFieldsReader> reader_;
    std::shared_ptr<const IndexSortDocMap> doc_map_;
};

/// Term vectors presented in index-sorted order.
///
/// Port of the term vectors half of `sorting_codec_reader.rs`
/// (`core/index/sorting_codec_reader.rs`).
export class SortingTermVectorsReader final : public TermVectorsReader {
public:
    /// Wraps `reader`; both arguments must outlive the wrapper.
    [[nodiscard]] static Result<std::unique_ptr<SortingTermVectorsReader>> create(
            std::shared_ptr<TermVectorsReader> reader, std::shared_ptr<const IndexSortDocMap> doc_map);

    // ---- TermVectors ----

    [[nodiscard]] VoidResult prefetch(int32_t doc_id) override;
    [[nodiscard]] Result<std::unique_ptr<Fields>> get(int32_t doc) override;

    // ---- TermVectorsReader ----

    [[nodiscard]] VoidResult check_integrity() const override;
    [[nodiscard]] Result<std::unique_ptr<TermVectorsReader>> get_merge_instance() const override;
    [[nodiscard]] Result<std::unique_ptr<TermVectorsReader>> clone() const override;
    [[nodiscard]] VoidResult close() const override;

private:
    SortingTermVectorsReader(std::shared_ptr<TermVectorsReader> reader,
                             std::shared_ptr<const IndexSortDocMap> doc_map);

    /// The document of the wrapped reader that `doc_id` stands for.
    [[nodiscard]] Result<int32_t> old_doc(int32_t doc_id) const;

    std::shared_ptr<TermVectorsReader> reader_;
    std::shared_ptr<const IndexSortDocMap> doc_map_;
};

} // namespace lucene
