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

export module lucene_core:frozen_buffered_updates;

import :accountable;
import :buffered_updates;
import :bytes_ref;
import :error;
import :field_updates_buffer;
import :prefix_coded_terms;
import :query;
import :types;

import std;

namespace lucene {

/// Buffered deletes and updates, frozen into a write-once packet.
///
/// Port of `index/frozen_buffered_updates.rs`.  Freezing compresses the deleted
/// terms into a [`PrefixCodedTerms`] (shared prefixes, sorted field-then-term
/// order) and takes ownership of the buffered queries and doc values updates, so
/// the packet is what the [`BufferedUpdatesStream`](:buffered_updates_stream)
/// hands around.
///
/// **Deviations:**
///  * the reference implementation drops the per-term document limits and
///    decides whether a delete applies to a segment by comparing the packet's
///    generation with the segment's; the port applies every packet to every
///    segment it still covers, so it keeps [`term_limits`](Self::term_limits)
///    (the document count each delete was issued at) next to the terms;
///  * `apply` is not here: the port's writer resolves the documents of a packet
///    itself (`IndexWriterImpl::apply_deletes_to`) because its deletion
///    bookkeeping is per commit rather than per generation.
export class FrozenBufferedUpdates : public Accountable {
public:
    /// Freezes `updates`.  The caller must not use `updates` afterwards: its
    /// term list, queries and doc values updates are moved into the packet.
    [[nodiscard]] static Result<std::shared_ptr<FrozenBufferedUpdates>> freeze(
            std::unique_ptr<BufferedUpdates> updates, std::optional<std::string> private_segment);

    /// The deleted terms, sorted field-then-term.
    [[nodiscard]] const PrefixCodedTerms &delete_terms() const noexcept { return delete_terms_; }

    /// The document count each deleted term was issued at, parallel to
    /// [`delete_terms`](Self::delete_terms) iteration order.
    [[nodiscard]] const std::vector<int32_t> &term_limits() const noexcept { return term_limits_; }

    /// The deleted queries.
    [[nodiscard]] const std::vector<std::shared_ptr<const Query>> &delete_queries() const noexcept {
        return delete_queries_;
    }

    /// The document count each deleted query was issued at.
    [[nodiscard]] const std::vector<int32_t> &delete_query_limits() const noexcept { return delete_query_limits_; }

    /// The buffered doc values updates, by field.
    [[nodiscard]] const std::map<std::string, FieldUpdatesBuffer, std::less<>> &field_updates() const noexcept {
        return field_updates_;
    }

    /// Number of buffered doc values updates.
    [[nodiscard]] int32_t field_updates_count() const noexcept { return field_updates_count_; }

    /// Bytes the packet holds (an estimate, from the buffered updates' accounting).
    [[nodiscard]] int32_t bytes_used() const noexcept { return bytes_used_; }

    [[nodiscard]] int64_t ram_bytes_used() const override { return bytes_used_; }

    /// The deletion generation [`BufferedUpdatesStream::push`](:buffered_updates_stream)
    /// assigned, or `-1` before it was pushed.
    [[nodiscard]] int64_t del_gen() const noexcept { return del_gen_; }
    void set_del_gen(int64_t del_gen) noexcept { del_gen_ = del_gen; }

    [[nodiscard]] bool is_applied() const noexcept { return applied_; }
    void mark_applied() noexcept { applied_ = true; }

    /// Documents this packet deleted or updated; filled in while it is applied.
    [[nodiscard]] int64_t total_del_count() const noexcept { return total_del_count_; }
    void add_deleted(int64_t count) noexcept { total_del_count_ += count; }

    /// The segment the packet is private to, when it was frozen for one segment.
    [[nodiscard]] const std::optional<std::string> &private_segment() const noexcept { return private_segment_; }

    [[nodiscard]] std::string to_string() const;

private:
    FrozenBufferedUpdates(PrefixCodedTerms delete_terms, std::vector<int32_t> term_limits,
                          std::vector<std::shared_ptr<const Query>> delete_queries,
                          std::vector<int32_t> delete_query_limits,
                          std::map<std::string, FieldUpdatesBuffer, std::less<>> field_updates,
                          int32_t field_updates_count, int32_t bytes_used, std::optional<std::string> private_segment)
        : delete_terms_(std::move(delete_terms)),
          term_limits_(std::move(term_limits)),
          delete_queries_(std::move(delete_queries)),
          delete_query_limits_(std::move(delete_query_limits)),
          field_updates_(std::move(field_updates)),
          field_updates_count_(field_updates_count),
          bytes_used_(bytes_used),
          private_segment_(std::move(private_segment)) {}

    PrefixCodedTerms delete_terms_;
    std::vector<int32_t> term_limits_;
    std::vector<std::shared_ptr<const Query>> delete_queries_;
    std::vector<int32_t> delete_query_limits_;
    std::map<std::string, FieldUpdatesBuffer, std::less<>> field_updates_;
    int32_t field_updates_count_{0};
    int32_t bytes_used_{0};
    int64_t del_gen_{-1};
    bool applied_{false};
    int64_t total_del_count_{0};
    std::optional<std::string> private_segment_;
};

} // namespace lucene
