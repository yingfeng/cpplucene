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

export module lucene_core:phrase_positions;

import :types;
import std;

import :error;
import :postings_enum;

namespace lucene {

/// The position of one phrase term in a document, normalized by the term's
/// offset inside the phrase.
///
/// Port of `PhrasePositions` (`core/search/phrase_positions.rs`).
///
/// [`position`] is `location - offset`, so an exact phrase is found when every
/// [`PhrasePositions`] of the phrase has the same [`position`].  The postings are
/// owned by the matcher and passed in on every call, hence `postings_idx` rather
/// than a pointer.
export class PhrasePositions {
public:
    /// A position for the `postings_idx`-th postings of the matcher, at
    /// `offset` inside the phrase.
    PhrasePositions(size_t postings_idx, int32_t offset, size_t ord);

    /// Position in the document, normalized by [`offset`].
    [[nodiscard]] int32_t position() const noexcept { return position_; }
    /// How many positions are left in the current document.
    [[nodiscard]] int32_t count() const noexcept { return count_; }
    /// Position inside the phrase.
    [[nodiscard]] int32_t offset() const noexcept { return offset_; }
    /// Unique ordinal across all [`PhrasePositions`] of one matcher.
    [[nodiscard]] size_t ord() const noexcept { return ord_; }
    /// Index of the postings this position reads.
    [[nodiscard]] size_t postings_idx() const noexcept { return postings_idx_; }
    /// The repetition group this position belongs to, when its term repeats in
    /// the phrase.
    [[nodiscard]] const std::optional<size_t> &rpt_group() const noexcept { return rpt_group_; }
    /// Index inside the repetition group.
    [[nodiscard]] size_t rpt_ind() const noexcept { return rpt_ind_; }

    void set_repetition(std::optional<size_t> group, size_t index) noexcept {
        rpt_group_ = group;
        rpt_ind_ = index;
    }

    /// Reads the first position of the current document.
    [[nodiscard]] VoidResult first_position(PostingsEnum &postings);

    /// Moves to the next location of this term in the current document; `false`
    /// when the document has no more positions.
    [[nodiscard]] Result<bool> next_position(PostingsEnum &postings);

private:
    size_t postings_idx_{0};
    /// Position in the document, normalized by [`offset`].
    int32_t position_{0};
    /// Remaining positions in this document.
    int32_t count_{0};
    /// Position in the phrase.
    int32_t offset_{0};
    /// Unique ordinal across all positions of a matcher.
    size_t ord_{0};
    /// Repetition group, when the term repeats.
    std::optional<size_t> rpt_group_{};
    /// Index inside the repetition group.
    size_t rpt_ind_{0};
};

} // namespace lucene
