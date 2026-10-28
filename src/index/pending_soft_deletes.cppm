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

export module lucene_core:pending_soft_deletes;

import :types;
import std;

import :bits;
import :doc_id_set_iterator;
import :doc_values;
import :error;
import :fixed_bit_set;
import :leaf_reader;

namespace lucene {

/// The soft deletes of one segment, read from the soft deletes field.
///
/// Port of the parts of `PendingSoftDeletes` (`index/pending_soft_deletes.rs`)
/// that do not need the deletion bookkeeping: the port keeps the delete counts in
/// [`SegmentCommitInfo`](lucene_core:segment_commit_info), so what is left here is
/// the answer to "which documents were soft deleted, and when".
///
/// The generation of a soft delete is the value the writer left in the field, so a
/// retention policy can compare it against a retention age and decide which
/// documents may be dropped for good.
///
/// **Deviation:** the reference implementation treats *any* value of the field as
/// a soft delete and writes it only on the deleted documents; this port writes the
/// field on every version and treats `0` as "live", which is what
/// [`IndexWriter::soft_update_document`](lucene_core:index_writer) requires of the
/// document it is given.  A document without a value counts as live.
export class PendingSoftDeletes {
public:
    /// Reads the soft deletes field of `reader`; a reader without the field has
    /// none.
    [[nodiscard]] static Result<PendingSoftDeletes> create(std::string field, LeafReader &reader);

    /// The field the deletes are recorded in.
    [[nodiscard]] const std::string &field() const noexcept { return field_; }

    /// How many documents are soft deleted, hard deleted ones included.
    [[nodiscard]] int32_t num_soft_deletes() const noexcept {
        return static_cast<int32_t>(generations_.size());
    }

    /// `true` when the segment has no soft deleted document.
    [[nodiscard]] bool is_empty() const noexcept { return generations_.empty(); }

    /// How many soft deletes are at or below `generation`, which is how many a
    /// retention policy that keeps the younger ones would drop.
    [[nodiscard]] int32_t num_soft_deletes_below(int64_t generation) const noexcept;

    /// The smallest generation, or `0` when there is none.
    [[nodiscard]] int64_t min_generation() const noexcept;

    /// The largest generation, or `0` when there is none.
    [[nodiscard]] int64_t max_generation() const noexcept;

    /// The documents whose soft delete is at or below `generation`, in document
    /// order.
    [[nodiscard]] std::vector<int32_t> doc_ids_below(int64_t generation) const;

    /// The generation of `doc`, or `0` when it is not soft deleted.
    [[nodiscard]] int64_t generation_of(int32_t doc) const noexcept;

    /// Rewrites `live` from the doc values: a document with a non-zero value is
    /// cleared (it is soft deleted), one with `0` is set again (its delete expires).
    ///
    /// Port of `apply_soft_deletes` (`index/pending_soft_deletes.rs`).  `live` must
    /// already exclude the hard deletes, which this function does not touch: a
    /// hard deleted document has no value any more in practice, but reviving it
    /// would be wrong, so it is left alone when it carries no soft delete.
    ///
    /// Answers how many documents were deleted minus the ones that were revived.
    [[nodiscard]] static Result<int32_t> apply_soft_deletes(NumericDocValues &values, FixedBitSet &live);

    /// Counts the soft deleted documents that are not hard deleted.
    ///
    /// Port of `count_soft_deletes` (`index/pending_soft_deletes.rs`).
    [[nodiscard]] static Result<int32_t> count_soft_deletes(DocIdSetIterator *soft_deleted,
                                                            const Bits *hard_deletes);

private:
    PendingSoftDeletes(std::string field, std::vector<std::pair<int32_t, int64_t>> generations)
            : field_(std::move(field)), generations_(std::move(generations)) {}

    std::string field_;
    /// `(doc, generation)` of every soft deleted document, in document order.
    std::vector<std::pair<int32_t, int64_t>> generations_;
};

} // namespace lucene
