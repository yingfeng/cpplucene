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

export module lucene_core:term_matches_iterator;

import :types;
import std;

import :error;
import :matches_iterator;
import :postings_enum;
import :query;

namespace lucene {

/// A [`MatchesIterator`] over the postings of one term, positioned on a single
/// document.
///
/// Port of `TermMatchesIterator` (`core/search/term_matches_iterator.rs`).
///
/// The postings must have been opened with the `POSITIONS` flag, and with the
/// `OFFSETS` flag for [`start_offset`] and [`end_offset`] to answer anything but
/// `-1`.
export class TermMatchesIterator final : public MatchesIterator {
public:
    /// An iterator over the positions of `postings`, which must be positioned on
    /// the document of interest.
    [[nodiscard]] static Result<std::unique_ptr<TermMatchesIterator>>
    create(std::unique_ptr<PostingsEnum> postings, std::shared_ptr<Query> query);

    // ---- MatchesIterator ----

    [[nodiscard]] Result<bool> next() override;
    [[nodiscard]] Result<int32_t> start_position() const override { return pos_; }
    [[nodiscard]] Result<int32_t> end_position() const override { return pos_; }
    [[nodiscard]] Result<int32_t> start_offset() const override { return postings_->start_offset(); }
    [[nodiscard]] Result<int32_t> end_offset() const override { return postings_->end_offset(); }

    /// A single term has no sub-matches.
    [[nodiscard]] Result<std::unique_ptr<MatchesIterator>> get_sub_matches() override {
        return std::unique_ptr<MatchesIterator>{};
    }

    [[nodiscard]] Result<std::shared_ptr<Query>> get_query() const override { return query_; }

private:
    TermMatchesIterator(std::unique_ptr<PostingsEnum> postings, std::shared_ptr<Query> query, int32_t upto);

    /// Positions left in the current document.
    int32_t upto_{0};
    /// The position of the current match.
    int32_t pos_{0};
    std::unique_ptr<PostingsEnum> postings_;
    std::shared_ptr<Query> query_;
};

} // namespace lucene
