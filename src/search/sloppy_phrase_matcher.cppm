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

export module lucene_core:sloppy_phrase_matcher;

import :types;
import std;

import :error;
import :phrase_matcher;
import :phrase_positions;
import :postings_enum;

namespace lucene {

/// Matches a phrase whose terms may be up to `slop` positions apart, scoring
/// every match by how tight it is (`1 / (1 + match_length)`).
///
/// Port of `SloppyPhraseMatcher` (`core/search/sloppy_phrase_matcher.rs`).
///
/// The smallest current position leads; it is advanced until it passes the
/// second smallest, which minimizes the width of the match.  When a term repeats
/// in the phrase, the repeated positions must not collide: the smaller of the
/// two colliding positions is advanced until the collision is resolved.
///
/// **Deviation:** the reference implementation derives the repetition groups
/// from a bipartite graph of postings and terms to also catch collisions hidden
/// behind multi-term postings; this port has no multi-term postings, so the
/// groups are simply the phrase slots that share a term.
export class SloppyPhraseMatcher final : public PhraseMatcherBase {
public:
    /// A matcher for a phrase with `slop` allowed movement.  `expose_offsets`
    /// makes the matcher read start and end offsets, which the postings only
    /// carry when they were opened with the `OFFSETS` flag.
    SloppyPhraseMatcher(std::vector<PostingsAndFreq> postings, size_t slop, float match_cost, bool expose_offsets);

    // ---- PhraseMatcher ----

    [[nodiscard]] Result<float> max_freq() override;
    [[nodiscard]] VoidResult reset() override;
    [[nodiscard]] Result<bool> next_match() override;
    [[nodiscard]] float sloppy_weight() const override;
    [[nodiscard]] int32_t start_position() const override;
    [[nodiscard]] int32_t end_position() const override;
    [[nodiscard]] Result<int32_t> start_offset() const override;
    [[nodiscard]] Result<int32_t> end_offset() const override;
    [[nodiscard]] float get_match_cost() const override { return match_cost_; }

private:
    /// Places the positions of the current document, finding the repetition
    /// groups the first time; `false` when the document cannot match.
    [[nodiscard]] Result<bool> init_phrase_positions();
    /// The first call: detects repetitions and initializes accordingly.
    [[nodiscard]] Result<bool> init_first_time();
    /// The initialization for a phrase without repetitions.
    [[nodiscard]] VoidResult init_simple();
    /// The initialization for a phrase with repetitions.
    [[nodiscard]] Result<bool> init_complex();
    /// Reads the first position of every slot.
    [[nodiscard]] VoidResult place_first_positions();
    /// Rebuilds the position queue from the current positions.
    [[nodiscard]] VoidResult fill_queue();
    /// Moves the repeated slots of each group so that they do not start colliding.
    [[nodiscard]] Result<bool> advance_repeat_groups();

    /// Advances one slot to its next position and updates [`end`].
    [[nodiscard]] Result<bool> advance_pp(size_t pp_index);
    /// Resolves the collisions of the repetition group `pp_index` belongs to.
    [[nodiscard]] Result<bool> advance_rpts(size_t pp_index);
    /// The slot index inside `pp_index`'s group that collides with it.
    [[nodiscard]] Result<std::optional<size_t>> collide(size_t pp_index);
    /// The slot that sorts first by `(position, offset)`.
    [[nodiscard]] size_t lesser(size_t left, size_t right) const;

    /// `position + offset` of a slot.
    [[nodiscard]] int32_t tp_pos(size_t pp_index) const;
    /// Records the current match for the `matches()` entry points.
    void capture_lead(size_t pp_index);

    // ---- the position queue, a min heap on `(position, offset, ord)` ----
    [[nodiscard]] bool less_slot(size_t left, size_t right) const;
    void queue_add(size_t pp_index);
    /// Pops the smallest slot; the queue must not be empty.
    [[nodiscard]] size_t queue_pop();
    [[nodiscard]] size_t queue_top() const { return queue_.front(); }

    size_t slop_{0};
    /// The slots, in the order the matcher's postings are.
    std::vector<PhrasePositions> pps_;
    /// A min heap of slot indices, by `(position, offset, ord)`.
    std::vector<size_t> queue_;
    /// Scratch stack for re-queueing after a repetition collision.
    std::vector<size_t> rpt_stack_;
    /// Marks the slots of a repetition group that have to be re-queued.
    std::vector<bool> rpt_bits_;
    /// Slots that share a term, sorted by position inside the phrase.
    std::vector<std::vector<size_t>> rpt_groups_;
    /// The largest position seen in the current document.
    int32_t end_{std::numeric_limits<int32_t>::min()};
    /// The width of the current match, `end - lead_position`.
    int32_t match_length_{0};
    int32_t lead_position_{0};
    int32_t lead_offset_{0};
    int32_t lead_end_offset_{0};
    size_t lead_ord_{0};
    /// Whether the phrase has repeated terms.
    bool has_rpts_{false};
    /// Whether the repetitions have been looked for.
    bool checked_rpts_{false};
    /// Whether the current document has its positions placed.
    bool positioned_{false};
    /// Whether start and end offsets are read as well.
    bool capture_lead_match_{false};
    float match_cost_{0.0F};
};

} // namespace lucene
