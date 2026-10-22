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

export module lucene_core:base_composite_reader;

import :types;
import std;

import :error;
import :fields;
import :index_reader;
import :leaf_reader_context;
import :stored_fields;
import :term;
import :term_vectors;

namespace lucene {

/// The index of the sub-reader that serves document `doc_id`.
///
/// Port of `reader_index` (`index/base_composite_reader.rs`): validates the
/// document number against `max_doc` and then binary-searches `starts`.
export [[nodiscard]] Result<int32_t> reader_index(int32_t doc_id, int32_t max_doc, std::span<const int32_t> starts);

/// [`StoredFields`] over several sub-readers, mapping a global document number
/// to `(sub-reader, local document number)`.
///
/// Port of `StoredFieldsImpl` (`index/base_composite_reader.rs`).  The
/// per-sub-reader cursors are opened lazily, on the first document that needs
/// them, because a composite reader is often created without ever reading the
/// stored fields.
export class MultiStoredFields final : public StoredFields {
public:
    MultiStoredFields(std::vector<std::shared_ptr<IndexReader>> sub_readers, std::vector<int32_t> starts,
                      int32_t max_doc);

    [[nodiscard]] VoidResult prefetch(int32_t doc_id) override;
    [[nodiscard]] VoidResult document_with_visitor(int32_t doc_id, StoredFieldVisitor &visitor) override;
    [[nodiscard]] Result<Document> document(int32_t doc_id) override;

private:
    [[nodiscard]] Result<StoredFields *> cursor_for(int32_t doc_id, int32_t &local_doc);

    std::vector<std::shared_ptr<IndexReader>> sub_readers_;
    std::vector<int32_t> starts_;
    std::vector<std::unique_ptr<StoredFields>> cursors_;
    int32_t max_doc_;
};

/// [`TermVectors`] over several sub-readers; the counterpart of
/// [`MultiStoredFields`] for term vectors.
export class MultiTermVectors final : public TermVectors {
public:
    MultiTermVectors(std::vector<std::shared_ptr<IndexReader>> sub_readers, std::vector<int32_t> starts,
                     int32_t max_doc);

    [[nodiscard]] VoidResult prefetch(int32_t doc_id) override;
    [[nodiscard]] Result<std::unique_ptr<Fields>> get(int32_t doc) override;

private:
    [[nodiscard]] Result<TermVectors *> cursor_for(int32_t doc_id, int32_t &local_doc);

    std::vector<std::shared_ptr<IndexReader>> sub_readers_;
    std::vector<int32_t> starts_;
    std::vector<std::unique_ptr<TermVectors>> cursors_;
    int32_t max_doc_;
};

/// The shared state of a composite reader: its ordered sub-readers, the first
/// document number of each, and the aggregated statistics.
///
/// Port of `BaseCompositeReaderBase` (`index/base_composite_reader.rs`), minus
/// the parent-reader registry and the reference counting, which this port
/// replaces with `std::shared_ptr`.
///
/// The `starts` array has one extra entry (`sub_readers.size() + 1`) holding
/// `max_doc`, which is what `reader_index` searches in.
export class CompositeReaderBase {
public:
    /// An empty base; use [`create`] to build a usable one.
    ///
    /// Public only so that reader types can hold it by value and fill it in
    /// after their own construction; the state stays private.
    CompositeReaderBase() = default;

    /// Wraps `sub_readers`, computing the document bases.
    ///
    /// Fails with `illegal_argument` when the sub-readers hold more than
    /// [`SegmentInfos::MAX_DOCS`] documents in total.
    [[nodiscard]] static Result<CompositeReaderBase>
    create(std::vector<std::shared_ptr<IndexReader>> sub_readers);

    [[nodiscard]] const std::vector<std::shared_ptr<IndexReader>> &sub_readers() const noexcept {
        return sub_readers_;
    }

    /// One entry per sub-reader plus the trailing `max_doc`.
    [[nodiscard]] const std::vector<int32_t> &starts() const noexcept { return starts_; }

    [[nodiscard]] int32_t max_doc() const noexcept { return max_doc_; }

    /// Sum of the sub-readers' live document counts.
    ///
    /// Unlike the reference implementation this is not cached: the port has no atomics in the reader
    /// tree, and the sub-readers cache their own counts anyway.
    [[nodiscard]] Result<int32_t> num_docs() const;

    /// First document number of sub-reader `index`.
    [[nodiscard]] Result<int32_t> reader_base(size_t index) const;

    // ---- statistics, aggregated over the sub-readers ----

    [[nodiscard]] Result<int32_t> doc_freq(const Term &term) const;
    [[nodiscard]] Result<int64_t> total_term_freq(const Term &term) const;
    [[nodiscard]] Result<int64_t> sum_doc_freq(std::string_view field) const;
    [[nodiscard]] Result<int32_t> doc_count(std::string_view field) const;
    [[nodiscard]] Result<int64_t> sum_total_term_freq(std::string_view field) const;

    /// A stored fields cursor dispatching into the sub-readers.
    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() const;
    /// A term vectors cursor dispatching into the sub-readers.
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() const;

private:
    std::vector<std::shared_ptr<IndexReader>> sub_readers_;
    std::vector<int32_t> starts_;
    int32_t max_doc_{0};
};

} // namespace lucene
