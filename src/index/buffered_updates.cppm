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

export module lucene_core:buffered_updates;

import :accountable;
import :byte_block_pool;
import :bytes_ref;
import :bytes_ref_hash;
import :doc_values_update;
import :error;
import :field_updates_buffer;
import :query;
import :term;
import :types;

import std;

namespace lucene {

/// The terms a segment must drop, each with the document count the delete was
/// issued at.
///
/// Port of `DeletedTerms` (`index/buffered_updates.rs`).  Terms are stored in a
/// [`ByteBlockPool`] so that thousands of deletes stay compact, and the values
/// live in a parallel array indexed by [`BytesRefHash`] id.
export class DeletedTerms : public Accountable {
public:
    DeletedTerms() = default;

    /// Records that `term` was deleted when the index held `value` documents.
    ///
    /// Adding the same term twice with a *smaller* count is a no-op: the newest
    /// count wins, because a later delete also covers the documents an earlier
    /// one covered.
    [[nodiscard]] VoidResult put(const Term &term, int32_t value);

    /// The newest document count of `term`, or `-1` when it was never deleted.
    [[nodiscard]] Result<int32_t> get(const Term &term) const;

    /// Number of distinct deleted terms.
    [[nodiscard]] int32_t size() const noexcept { return terms_size_; }

    [[nodiscard]] bool is_empty() const noexcept { return terms_size_ == 0; }

    /// Drops every term and releases the pool.
    void clear();

    /// Every deleted term, sorted by field name and then by term bytes.
    ///
    /// **Deviation:** the reference implementation returns a `HashSet`; a sorted
    /// vector keeps the type copyable and hash-free.
    [[nodiscard]] Result<std::vector<Term>> key_set() const;

    /// Calls `consumer(field, bytes, doc_id_upto)` for every deleted term, field
    /// names in ascending order and terms in ascending byte order.
    ///
    /// Destructive: it sorts the internal hash, which invalidates the ids.
    using Consumer = std::function<VoidResult(std::string_view, const BytesRef &, int32_t)>;
    [[nodiscard]] VoidResult for_each_ordered(const Consumer &consumer);

    [[nodiscard]] int64_t ram_bytes_used() const override;

    [[nodiscard]] std::string to_string() const;

private:
    /// The deleted terms of one field: a hash over the pool plus one document
    /// count per id.
    struct FieldTerms {
        BytesRefHash hash;
        std::vector<int32_t> values;

        explicit FieldTerms(size_t capacity);
        FieldTerms(FieldTerms &&) noexcept = default;
        FieldTerms &operator=(FieldTerms &&) noexcept = default;
        FieldTerms(const FieldTerms &) = delete;
        FieldTerms &operator=(const FieldTerms &) = delete;
    };

    ByteBlockPool pool_;
    std::map<std::string, FieldTerms, std::less<>> delete_terms_;
    int32_t terms_size_{0};
    int64_t bytes_used_{0};
};

/// Buffered deletes and updates that apply to one segment (or, in the writer,
/// to everything indexed so far).
///
/// Port of `BufferedUpdates` (`index/buffered_updates.rs`).
///
/// **Deviations** (the port buffers deletes at the writer level and applies them
/// when the commit or the near-real-time read needs them, so this class is used
/// as a container rather than as the writer's only path):
///  * `delete_queries` is a vector compared with [`Query::equals`] instead of a
///    `HashMap<Query, i32>`: the port's `Query` is an abstract class, so it can
///    be hashed only through that method;
///  * the RAM accounting keeps running counters instead of the reference
///    implementation's `Accountable` walk over the map buckets.
export class BufferedUpdates : public Accountable {
public:
    BufferedUpdates() = default;

    /// Buffers an update of a binary doc values field.
    [[nodiscard]] VoidResult add_binary_update(const DocValuesUpdate &update, int32_t doc_id_upto);

    /// Buffers an update of a numeric doc values field.
    [[nodiscard]] VoidResult add_numeric_update(const DocValuesUpdate &update, int32_t doc_id_upto);

    /// Buffers the deletion of `term`.
    [[nodiscard]] VoidResult add_term(const Term &term, int32_t doc_id_upto);

    /// Buffers the deletion of every document `query` matches.
    [[nodiscard]] VoidResult add_query(std::shared_ptr<const Query> query, int32_t doc_id_upto);

    void clear_delete_terms() { delete_terms_.clear(); }

    /// Drops everything buffered and resets the counters.
    void clear();

    /// Whether anything is buffered.
    [[nodiscard]] bool any() const;

    /// Number of buffered doc values updates.
    [[nodiscard]] int32_t num_field_updates() const noexcept { return num_field_updates_; }

    /// Bytes of the buffered queries, doc values updates and deleted terms.
    [[nodiscard]] int64_t ram_bytes_used() const override {
        return bytes_used_ + field_updates_bytes_used_ + delete_terms_.ram_bytes_used();
    }

    /// The generation the packet was frozen with (0 while it is still open).
    [[nodiscard]] int64_t gen() const noexcept { return gen_; }
    void set_gen(int64_t gen) noexcept { gen_ = gen; }

    [[nodiscard]] DeletedTerms &delete_terms() noexcept { return delete_terms_; }
    [[nodiscard]] const DeletedTerms &delete_terms() const noexcept { return delete_terms_; }

    /// The buffered deletes by query, in insertion order.
    [[nodiscard]] const std::vector<std::pair<std::shared_ptr<const Query>, int32_t>> &delete_queries() const noexcept {
        return delete_queries_;
    }

    /// The buffered doc values updates, by field.
    [[nodiscard]] std::map<std::string, FieldUpdatesBuffer, std::less<>> &field_updates() noexcept {
        return field_updates_;
    }
    [[nodiscard]] const std::map<std::string, FieldUpdatesBuffer, std::less<>> &field_updates() const noexcept {
        return field_updates_;
    }

    /// Moves the buffered doc values updates out, leaving the map empty.
    ///
    /// Used when the updates are frozen into a packet, which owns them from then
    /// on (the reference implementation does the same with `mem::take`).
    [[nodiscard]] std::map<std::string, FieldUpdatesBuffer, std::less<>> take_field_updates() {
        std::map<std::string, FieldUpdatesBuffer, std::less<>> taken = std::move(field_updates_);
        field_updates_.clear();
        field_updates_bytes_used_ = 0;
        return taken;
    }

    /// Moves the buffered query deletes out, leaving the list empty.
    [[nodiscard]] std::vector<std::pair<std::shared_ptr<const Query>, int32_t>> take_delete_queries() {
        std::vector<std::pair<std::shared_ptr<const Query>, int32_t>> taken = std::move(delete_queries_);
        delete_queries_.clear();
        return taken;
    }

    /// `true` when every buffered delete and update was applied.
    [[nodiscard]] bool is_empty() const { return !any(); }

    [[nodiscard]] std::string to_string() const;

    /// Sets the verbosity of [`to_string`] (the reference implementation's
    /// `verbose_deletes`).
    void set_verbose_deletes(bool verbose) noexcept { verbose_deletes_ = verbose; }

private:
    /// Recomputes the doc values half of the RAM accounting after an insertion.
    void refresh_field_updates_bytes();

    int32_t num_field_updates_{0};
    DeletedTerms delete_terms_;
    std::vector<std::pair<std::shared_ptr<const Query>, int32_t>> delete_queries_;
    std::map<std::string, FieldUpdatesBuffer, std::less<>> field_updates_;
    int64_t bytes_used_{0};
    int64_t field_updates_bytes_used_{0};
    bool verbose_deletes_{false};
    int64_t gen_{0};
};

} // namespace lucene
