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

export module lucene_core:disjunction_matches_iterator;

import :types;
import std;

import :error;
import :matches_iterator;
import :priority_queue;
import :query;

namespace lucene {

/// Orders sub-iterators by their start position, then by their end position, so
/// that prefixes sort first.
///
/// Port of `DisjunctionMatchesIteratorPQCmp`
/// (`core/search/disjunction_matches_iterator.rs`).
export class DisjunctionMatchesIteratorCompare {
public:
    [[nodiscard]] Result<bool> less_than(const std::unique_ptr<MatchesIterator> &a,
                                         const std::unique_ptr<MatchesIterator> &b) const {
        auto a_start = a->start_position();
        if (!a_start.has_value()) {
            return std::unexpected(std::move(a_start.error()));
        }
        auto b_start = b->start_position();
        if (!b_start.has_value()) {
            return std::unexpected(std::move(b_start.error()));
        }
        if (a_start.value() == b_start.value()) {
            auto a_end = a->end_position();
            if (!a_end.has_value()) {
                return std::unexpected(std::move(a_end.error()));
            }
            auto b_end = b->end_position();
            if (!b_end.has_value()) {
                return std::unexpected(std::move(b_end.error()));
            }
            return a_end.value() < b_end.value();
        }
        return a_start.value() < b_start.value();
    }
};

/// A [`MatchesIterator`] that combines the matches of a set of sub-iterators.
///
/// Port of `DisjunctionMatchesIterator`
/// (`core/search/disjunction_matches_iterator.rs`).
///
/// Matches are sorted by their start positions, then by their end positions, so
/// that prefixes sort first.  Matches may overlap, or be duplicated when they
/// appear in more than one of the sub-iterators.
///
/// **Deviation:** the reference implementation also exposes
/// `TermsEnumDisjunctionMatchesIterator` / `TermBytesRefIterator`, which build a
/// disjunction out of a multi-term rewrite; the port only needs the plain
/// combination of finished iterators.
export class DisjunctionMatchesIterator final : public MatchesIterator {
public:
    /// Combines `matches`, or answers a null pointer when none of them has a
    /// first match.
    [[nodiscard]] static Result<std::unique_ptr<MatchesIterator>>
    from_sub_iterators(std::vector<std::unique_ptr<MatchesIterator>> matches);

    // ---- MatchesIterator ----

    [[nodiscard]] Result<bool> next() override;
    [[nodiscard]] Result<int32_t> start_position() const override;
    [[nodiscard]] Result<int32_t> end_position() const override;
    [[nodiscard]] Result<int32_t> start_offset() const override;
    [[nodiscard]] Result<int32_t> end_offset() const override;
    [[nodiscard]] Result<std::unique_ptr<MatchesIterator>> get_sub_matches() override;
    [[nodiscard]] Result<std::shared_ptr<Query>> get_query() const override;

private:
    explicit DisjunctionMatchesIterator(PriorityQueue<std::unique_ptr<MatchesIterator>, DisjunctionMatchesIteratorCompare> queue);

    /// The current match of every sub-iterator, least first.
    PriorityQueue<std::unique_ptr<MatchesIterator>, DisjunctionMatchesIteratorCompare> queue_;
    bool started_{false};
};

} // namespace lucene
