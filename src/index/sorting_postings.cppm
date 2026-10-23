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

export module lucene_core:sorting_postings;

import :types;
import std;

import :error;
import :index_sorter;
import :postings_enum;

namespace lucene {

/// One term's postings, presented in index-sorted order.
///
/// Port of `SortingPostingsEnum` (`core/index/freq_prox_terms_writer.rs`).
///
/// The underlying postings enumerate the segment's documents in insertion order;
/// this wrapper materializes them once, renumbers the documents through the index
/// sort and replays them in the new order.  Positions, offsets and payloads move
/// with their document.
///
/// **Deviation:** the reference implementation keeps the materialized copy in a
/// byte buffer it can slice; the port keeps typed vectors, since the values never
/// reach a file.
export class SortingPostingsEnum final : public PostingsEnum {
public:
    /// Captures the postings of `delegate`; `flags` says which of the positions,
    /// offsets and payloads the caller wants.
    [[nodiscard]] static Result<std::unique_ptr<SortingPostingsEnum>> create(
            std::unique_ptr<PostingsEnum> delegate, const IndexSortDocMap &doc_map, int32_t flags);

    // ---- DocIdSetIterator ----

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override;

    // ---- PostingsEnum ----

    [[nodiscard]] Result<int32_t> freq() override;
    [[nodiscard]] Result<int32_t> next_position() override;
    [[nodiscard]] Result<int32_t> start_offset() const override;
    [[nodiscard]] Result<int32_t> end_offset() const override;
    [[nodiscard]] Result<std::optional<std::span<const uint8_t>>> get_payload() const override;

private:
    /// The postings of one document, plus where its positions start.
    struct DocEntry {
        int32_t doc{0};
        int32_t freq{0};
        size_t positions_start{0};
    };

    SortingPostingsEnum(std::vector<DocEntry> docs, std::vector<int32_t> positions, std::vector<int32_t> starts,
                        std::vector<int32_t> ends, std::vector<std::vector<uint8_t>> payloads, bool has_offsets,
                        bool has_payloads);

    /// The entry the iterator currently sits on.
    [[nodiscard]] const DocEntry &current() const;

    std::vector<DocEntry> docs_;
    std::vector<int32_t> positions_;
    std::vector<int32_t> starts_;
    std::vector<int32_t> ends_;
    /// One payload per position; empty when the term has no payloads.
    std::vector<std::vector<uint8_t>> payloads_;
    bool has_offsets_{false};
    bool has_payloads_{false};

    size_t position_{0};
    /// Index into the current document's positions.
    size_t position_index_{0};
    int32_t doc_{-1};
};

/// One term's documents, presented in index-sorted order, without frequencies.
///
/// Port of `SortingDocsEnum` (`core/index/freq_prox_terms_writer.rs`).
///
/// Used for fields indexed without frequencies: only the document numbers have
/// to be renumbered, so the wrapper keeps a plain sorted array and reports a
/// frequency of 1, a position of -1 and no offsets or payloads.
///
/// **Deviation:** the reference implementation keeps the numbers a deleted
/// document maps to (`-1`), which would surface as a document id of `-1`; the
/// port drops them, the same way [`SortingPostingsEnum`] does.
export class SortingDocsEnum final : public PostingsEnum {
public:
    /// Captures the documents of `delegate`, renumbered through `doc_map` and
    /// sorted into new-document order.
    [[nodiscard]] static Result<std::unique_ptr<SortingDocsEnum>> create(std::unique_ptr<PostingsEnum> delegate,
                                                                        const IndexSortDocMap &doc_map);

    // ---- DocIdSetIterator ----

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override;

    // ---- PostingsEnum ----

    [[nodiscard]] Result<int32_t> freq() override;
    [[nodiscard]] Result<int32_t> next_position() override;
    [[nodiscard]] Result<int32_t> start_offset() const override;
    [[nodiscard]] Result<int32_t> end_offset() const override;
    [[nodiscard]] Result<std::optional<std::span<const uint8_t>>> get_payload() const override;

private:
    explicit SortingDocsEnum(std::vector<int32_t> docs);

    std::vector<int32_t> docs_;
    size_t position_{0};
    int32_t doc_{-1};
};

} // namespace lucene
