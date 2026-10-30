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

export module lucene_core:term_automaton_scorer;

import :types;
import std;

import :error;
import :numeric_doc_values;
import :postings_enum;
import :run_automaton;
import :scorable;
import :scorer;
import :similarity;

namespace lucene {

/// One term of a `TermAutomatonQuery`, together with its position cursor.
///
/// Port of `EnumAndScorer` (`core/search/term_automaton_scorer.rs`).  The queues inside
/// [`TermAutomatonScorer`] refer to these by index, so the vector holding them
/// must not be resized while a scorer is alive.
export struct EnumAndScorer {
    /// The automaton label this term carries.
    int32_t term_id{0};
    /// The postings, opened with positions.
    std::unique_ptr<PostingsEnum> pos_enum{};
    /// How many positions of the current document are still to be visited.
    int32_t pos_left{0};
    /// The position the cursor currently sits on.
    int32_t pos{0};
};

/// Scores a `TermAutomatonQuery` by running the automaton over the positions of
/// every document that contains at least one of the query's terms.
///
/// Port of `TermAutomatonScorer` (`core/search/term_automaton_scorer.rs`).
///
/// The scorer is a disjunction over the terms (it visits any document holding
/// one of them) followed by a merge-sort of the positions inside each document,
/// which is what tells whether — and how many times — the automaton matches.
///
/// **Deviations** (C++ mechanics only):
///  * the reference implementation is generic over the postings, the similarity
///    scorer and the norms; the port stores them behind `std::unique_ptr` /
///    `std::shared_ptr` because every scorer in the port is built through the
///    same type-erased [`Scorer`] interface;
///  * the reference implementation's `ApproximationDISI` / bit-set accessors are
///    the defaults of [`Scorer`]: this scorer has no approximation.
export class TermAutomatonScorer final : public Scorer {
public:
    /// `subs` must not be empty; `run_automaton` must have been built over an
    /// alphabet large enough to hold every `term_id` in `subs` (plus the
    /// wildcard id).
    TermAutomatonScorer(std::shared_ptr<const RunAutomaton> run_automaton, std::vector<EnumAndScorer> subs,
                        int32_t any_term_id, std::shared_ptr<const SimScorer> sim_scorer,
                        std::unique_ptr<NumericDocValues> norms);

    /// How many times the automaton matched in the current document; this is
    /// the frequency the similarity scores with.
    [[nodiscard]] int32_t freq() const noexcept { return freq_; }

    /// The per-term cursors, for `explain`; their `pos_enum` documents which
    /// terms are positioned on the current document.
    [[nodiscard]] const std::vector<EnumAndScorer> &subs() const noexcept { return subs_; }

    // ---- DocIdSetIterator ----

    [[nodiscard]] int32_t doc_id() const override;
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override;

    // ---- Scorable ----

    [[nodiscard]] Result<float> score() override;

    // ---- Scorer ----

    [[nodiscard]] Result<int32_t> scoring_doc_id() override;
    [[nodiscard]] Result<float> get_max_score(int32_t upto) override;
    [[nodiscard]] TwoPhaseState has_two_phase_iterator() const override;

private:
    /// Pops every cursor positioned on the lowest document, i.e. the current
    /// one, out of the document queue.
    [[nodiscard]] VoidResult pop_current_doc();
    /// Pushes the cursors popped by [`pop_current_doc`] back.
    void push_current_doc();
    /// Positions `subs[index]` at the first position of its current document.
    [[nodiscard]] VoidResult position_sub_on_doc(size_t index);
    /// Moves every cursor forward until one document matches, or the cursors are
    /// exhausted.
    [[nodiscard]] Result<int32_t> do_next();
    /// Drops every pending match before `pos`.
    void shift(int32_t pos);
    /// Merge-sorts the positions of the current document and runs the automaton
    /// over them, setting `freq_`.
    [[nodiscard]] VoidResult count_matches();

    /// One automaton-state set per position, relative to `pos_shift_`.
    struct PosState {
        /// Automaton states reached at this position; only the first `count`
        /// entries are meaningful.
        std::vector<int32_t> states{0, 0};
        /// How many states are live.
        size_t count{0};

        void add(int32_t state) {
            if (states.size() == count) {
                states.push_back(0);
            }
            states[count] = state;
            ++count;
        }
    };

    /// A min-heap of cursor indices ordered by the document they sit on.
    class DocIdQueue {
    public:
        [[nodiscard]] bool empty() const noexcept { return heap_.empty(); }
        /// The cursor on the lowest document, or `nullopt` when empty.
        [[nodiscard]] std::optional<size_t> top() const;
        void add(size_t index, const std::vector<EnumAndScorer> &subs);
        [[nodiscard]] Result<size_t> pop(const std::vector<EnumAndScorer> &subs);

    private:
        [[nodiscard]] int32_t doc_of(size_t index, const std::vector<EnumAndScorer> &subs) const;

        std::vector<size_t> heap_{};
    };

    /// A min-heap of cursor indices ordered by their current position.
    class PositionQueue {
    public:
        [[nodiscard]] bool empty() const noexcept { return heap_.empty(); }
        void add(size_t index, const std::vector<EnumAndScorer> &subs);
        [[nodiscard]] Result<size_t> pop(const std::vector<EnumAndScorer> &subs);

    private:
        [[nodiscard]] int32_t pos_of(size_t index, const std::vector<EnumAndScorer> &subs) const;

        std::vector<size_t> heap_{};
    };

    std::shared_ptr<const RunAutomaton> run_automaton_;
    std::vector<EnumAndScorer> subs_;
    /// The indices of `subs_` that sit on `doc_id_`.
    std::vector<size_t> subs_on_doc_{};
    DocIdQueue doc_id_queue_;
    PositionQueue pos_queue_;
    /// One state set per position, reused across documents.
    std::vector<PosState> positions_{};
    /// Document id the entries of `positions_` are relative to.
    int32_t pos_shift_{-1};
    /// `-1` when the automaton has no wildcard transition.
    int32_t any_term_id_{-1};
    std::shared_ptr<const SimScorer> sim_scorer_;
    std::unique_ptr<NumericDocValues> norms_;
    int64_t cost_{0};
    int32_t doc_id_{-1};
    int32_t freq_{0};
};

} // namespace lucene
