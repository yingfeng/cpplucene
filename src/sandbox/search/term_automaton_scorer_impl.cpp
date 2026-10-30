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

module lucene_core:term_automaton_scorer.impl;

import :term_automaton_scorer;

import std;

import :doc_id_set_iterator;
import :error;
import :numeric_doc_values;
import :postings_enum;
import :run_automaton;
import :scorable;
import :scorer;
import :similarity;

namespace lucene {

// ------------------------------------------------------------- the  queues

int32_t TermAutomatonScorer::DocIdQueue::doc_of(size_t index, const std::vector<EnumAndScorer> &subs) const {
    return subs[index].pos_enum->doc_id();
}

std::optional<size_t> TermAutomatonScorer::DocIdQueue::top() const {
    if (heap_.empty()) {
        return std::nullopt;
    }
    return heap_.front();
}

void TermAutomatonScorer::DocIdQueue::add(size_t index, const std::vector<EnumAndScorer> &subs) {
    heap_.push_back(index);
    size_t position = heap_.size() - 1;
    while (position > 0) {
        const size_t parent = (position - 1) / 2;
        if (doc_of(heap_[parent], subs) <= doc_of(heap_[position], subs)) {
            break;
        }
        std::swap(heap_[parent], heap_[position]);
        position = parent;
    }
}

Result<size_t> TermAutomatonScorer::DocIdQueue::pop(const std::vector<EnumAndScorer> &subs) {
    if (heap_.empty()) {
        return err(LuceneError::illegal_state("document id queue is empty"));
    }
    const size_t last = heap_.back();
    heap_.pop_back();
    if (heap_.empty()) {
        return last;
    }
    const size_t result = heap_.front();
    heap_.front() = last;
    size_t index = 0;
    while (true) {
        const size_t left = index * 2 + 1;
        if (left >= heap_.size()) {
            break;
        }
        const size_t right = left + 1;
        size_t child = left;
        if (right < heap_.size() && doc_of(heap_[right], subs) < doc_of(heap_[left], subs)) {
            child = right;
        }
        if (doc_of(heap_[index], subs) <= doc_of(heap_[child], subs)) {
            break;
        }
        std::swap(heap_[index], heap_[child]);
        index = child;
    }
    return result;
}

int32_t TermAutomatonScorer::PositionQueue::pos_of(size_t index, const std::vector<EnumAndScorer> &subs) const {
    return subs[index].pos;
}

void TermAutomatonScorer::PositionQueue::add(size_t index, const std::vector<EnumAndScorer> &subs) {
    heap_.push_back(index);
    size_t position = heap_.size() - 1;
    while (position > 0) {
        const size_t parent = (position - 1) / 2;
        if (pos_of(heap_[parent], subs) <= pos_of(heap_[position], subs)) {
            break;
        }
        std::swap(heap_[parent], heap_[position]);
        position = parent;
    }
}

Result<size_t> TermAutomatonScorer::PositionQueue::pop(const std::vector<EnumAndScorer> &subs) {
    if (heap_.empty()) {
        return err(LuceneError::illegal_state("position queue is empty"));
    }
    const size_t last = heap_.back();
    heap_.pop_back();
    if (heap_.empty()) {
        return last;
    }
    const size_t result = heap_.front();
    heap_.front() = last;
    size_t index = 0;
    while (true) {
        const size_t left = index * 2 + 1;
        if (left >= heap_.size()) {
            break;
        }
        const size_t right = left + 1;
        size_t child = left;
        if (right < heap_.size() && pos_of(heap_[right], subs) < pos_of(heap_[left], subs)) {
            child = right;
        }
        if (pos_of(heap_[index], subs) <= pos_of(heap_[child], subs)) {
            break;
        }
        std::swap(heap_[index], heap_[child]);
        index = child;
    }
    return result;
}

// ---------------------------------------------------------------- the scorer

TermAutomatonScorer::TermAutomatonScorer(std::shared_ptr<const RunAutomaton> run_automaton,
                                         std::vector<EnumAndScorer> subs, int32_t any_term_id,
                                         std::shared_ptr<const SimScorer> sim_scorer,
                                         std::unique_ptr<NumericDocValues> norms)
        : run_automaton_(std::move(run_automaton)), subs_(std::move(subs)), any_term_id_(any_term_id),
          sim_scorer_(std::move(sim_scorer)), norms_(std::move(norms)) {
    subs_on_doc_.reserve(subs_.size());
    // The estimate only drives skipping decisions, so an iterator that cannot
    // report its cost simply contributes nothing.
    for (const EnumAndScorer &sub : subs_) {
        auto reported = sub.pos_enum->cost();
        if (reported.has_value()) {
            cost_ += reported.value();
        }
    }
    // Four slots is what the reference implementation starts with; it grows on
    // demand in `count_matches`.
    positions_.resize(4);
    // Nothing has been iterated yet: every term counts as "on the current
    // document" so that the first `next_doc`/`advance` moves them all, and the
    // document queue starts empty.
    for (size_t index = 0; index < subs_.size(); ++index) {
        subs_on_doc_.push_back(index);
    }
}

int32_t TermAutomatonScorer::doc_id() const { return doc_id_; }

VoidResult TermAutomatonScorer::pop_current_doc() {
    auto first = doc_id_queue_.pop(subs_);
    if (!first.has_value()) {
        return std::unexpected(std::move(first.error()));
    }
    doc_id_ = subs_[first.value()].pos_enum->doc_id();
    subs_on_doc_.push_back(first.value());
    while (true) {
        const std::optional<size_t> top = doc_id_queue_.top();
        if (!top.has_value()) {
            break;
        }
        if (subs_[top.value()].pos_enum->doc_id() != doc_id_) {
            break;
        }
        auto popped = doc_id_queue_.pop(subs_);
        if (!popped.has_value()) {
            return std::unexpected(std::move(popped.error()));
        }
        subs_on_doc_.push_back(popped.value());
    }
    return {};
}

void TermAutomatonScorer::push_current_doc() {
    for (const size_t index : subs_on_doc_) {
        doc_id_queue_.add(index, subs_);
    }
    subs_on_doc_.clear();
}

VoidResult TermAutomatonScorer::position_sub_on_doc(size_t index) {
    EnumAndScorer &sub = subs_[index];
    if (sub.pos_enum->doc_id() != NO_MORE_DOCS) {
        auto frequency = sub.pos_enum->freq();
        if (!frequency.has_value()) {
            return std::unexpected(std::move(frequency.error()));
        }
        sub.pos_left = frequency.value() - 1;
        auto position = sub.pos_enum->next_position();
        if (!position.has_value()) {
            return std::unexpected(std::move(position.error()));
        }
        sub.pos = position.value();
    }
    return {};
}

Result<int32_t> TermAutomatonScorer::do_next() {
    while (true) {
        if (auto popped = pop_current_doc(); !popped.has_value()) {
            return std::unexpected(std::move(popped.error()));
        }
        if (doc_id_ == NO_MORE_DOCS) {
            return doc_id_;
        }
        if (auto counted = count_matches(); !counted.has_value()) {
            return std::unexpected(std::move(counted.error()));
        }
        if (freq_ > 0) {
            return doc_id_;
        }
        for (size_t i = 0; i < subs_on_doc_.size(); ++i) {
            const size_t index = subs_on_doc_[i];
            auto next = subs_[index].pos_enum->next_doc();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
            if (auto positioned = position_sub_on_doc(index); !positioned.has_value()) {
                return std::unexpected(std::move(positioned.error()));
            }
        }
        push_current_doc();
    }
}

void TermAutomatonScorer::shift(int32_t pos) {
    const size_t limit = static_cast<size_t>(pos - pos_shift_);
    for (size_t i = 0; i < limit && i < positions_.size(); ++i) {
        positions_[i].count = 0;
    }
    pos_shift_ = pos;
}

VoidResult TermAutomatonScorer::count_matches() {
    freq_ = 0;
    for (const size_t index : subs_on_doc_) {
        pos_queue_.add(index, subs_);
    }

    int32_t last_pos = -1;
    pos_shift_ = -1;

    while (!pos_queue_.empty()) {
        auto popped = pos_queue_.pop(subs_);
        if (!popped.has_value()) {
            return std::unexpected(std::move(popped.error()));
        }
        const size_t index = popped.value();

        // A graph intersection: `pos` is the state this token leaves from.  The
        // token always transitions from `pos` to `pos + 1`, because the position
        // length of a token is not carried by the postings.
        const int32_t pos = subs_[index].pos;

        if (pos_shift_ == -1) {
            pos_shift_ = pos;
        }

        const size_t needed = static_cast<size_t>(pos + 1 - pos_shift_);
        // `needed + 1` is the next position as well, so both must be in range.
        if (needed + 1 >= positions_.size()) {
            positions_.resize(needed + 2 + needed / 8);
        }

        // Maybe advance ANY matches: every position between `last_pos` and `pos`
        // also holds the wildcard term.
        if (last_pos != -1 && any_term_id_ != -1) {
            const int32_t start_last_pos = last_pos;
            while (last_pos < pos) {
                const size_t current_index = static_cast<size_t>(last_pos - pos_shift_);
                if (positions_[current_index].count == 0 && last_pos > start_last_pos) {
                    // Petered out.
                    last_pos = pos;
                    break;
                }
                PosState &current = positions_[current_index];
                PosState &next = positions_[current_index + 1];
                for (size_t state_index = 0; state_index < current.count; ++state_index) {
                    const int32_t state = run_automaton_->step(current.states[state_index], any_term_id_).value_or(-1);
                    if (state != -1) {
                        next.add(state);
                    }
                }
                ++last_pos;
            }
        }

        size_t current_index = static_cast<size_t>(pos - pos_shift_);

        // If there are no pending matches at either this position or the next
        // one, it is safe to shift back to `positions_[0]`.
        if (positions_[current_index].count == 0 && positions_[current_index + 1].count == 0) {
            shift(pos);
        }

        current_index = static_cast<size_t>(pos - pos_shift_);
        PosState &current = positions_[current_index];
        PosState &next = positions_[current_index + 1];
        EnumAndScorer &sub = subs_[index];

        // Match the current token.
        for (size_t state_index = 0; state_index < current.count; ++state_index) {
            auto stepped = run_automaton_->step(current.states[state_index], sub.term_id);
            if (!stepped.has_value()) {
                return std::unexpected(std::move(stepped.error()));
            }
            const int32_t state = stepped.value();
            if (state != -1) {
                next.add(state);
                auto accept = run_automaton_->is_accept(state);
                if (!accept.has_value()) {
                    return std::unexpected(std::move(accept.error()));
                }
                if (accept.value()) {
                    ++freq_;
                }
            }
        }

        // Also consider starting a new match from this position.
        auto started = run_automaton_->step(0, sub.term_id);
        if (!started.has_value()) {
            return std::unexpected(std::move(started.error()));
        }
        if (started.value() != -1) {
            next.add(started.value());
            auto accept = run_automaton_->is_accept(started.value());
            if (!accept.has_value()) {
                return std::unexpected(std::move(accept.error()));
            }
            if (accept.value()) {
                ++freq_;
            }
        }

        if (sub.pos_left > 0) {
            // Put this sub back into the position queue.
            auto position = sub.pos_enum->next_position();
            if (!position.has_value()) {
                return std::unexpected(std::move(position.error()));
            }
            sub.pos = position.value();
            --sub.pos_left;
            pos_queue_.add(index, subs_);
        }

        last_pos = pos;
    }

    const size_t limit = static_cast<size_t>(last_pos + 1 - pos_shift_);
    for (size_t i = 0; i <= limit && i < positions_.size(); ++i) {
        positions_[i].count = 0;
    }
    return {};
}

Result<int32_t> TermAutomatonScorer::next_doc() {
    // Only the cursors positioned on the current document have to move: every
    // document in the queue is already beyond it.
    for (size_t i = 0; i < subs_on_doc_.size(); ++i) {
        const size_t index = subs_on_doc_[i];
        auto next = subs_[index].pos_enum->next_doc();
        if (!next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
        if (auto positioned = position_sub_on_doc(index); !positioned.has_value()) {
            return std::unexpected(std::move(positioned.error()));
        }
    }
    push_current_doc();
    return do_next();
}

Result<int32_t> TermAutomatonScorer::advance(int32_t target) {
    // Both the positioned cursors and the ones in the queue may be behind
    // `target`.

    // 1. Advance the queue.
    while (true) {
        const std::optional<size_t> top = doc_id_queue_.top();
        if (!top.has_value()) {
            break;
        }
        if (subs_[top.value()].pos_enum->doc_id() >= target) {
            break;
        }
        auto popped = doc_id_queue_.pop(subs_);
        if (!popped.has_value()) {
            return std::unexpected(std::move(popped.error()));
        }
        const size_t index = popped.value();
        auto advanced = subs_[index].pos_enum->advance(target);
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        if (auto positioned = position_sub_on_doc(index); !positioned.has_value()) {
            return std::unexpected(std::move(positioned.error()));
        }
        doc_id_queue_.add(index, subs_);
    }

    // 2. Advance the cursors positioned on the current document.
    for (size_t i = 0; i < subs_on_doc_.size(); ++i) {
        const size_t index = subs_on_doc_[i];
        auto advanced = subs_[index].pos_enum->advance(target);
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        if (auto positioned = position_sub_on_doc(index); !positioned.has_value()) {
            return std::unexpected(std::move(positioned.error()));
        }
    }
    push_current_doc();
    return do_next();
}

Result<int64_t> TermAutomatonScorer::cost() const { return cost_; }

Result<float> TermAutomatonScorer::score() {
    int64_t norm = 1;
    if (norms_ != nullptr) {
        auto exact = norms_->advance_exact(doc_id_);
        if (!exact.has_value()) {
            return std::unexpected(std::move(exact.error()));
        }
        if (exact.value()) {
            auto value = norms_->long_value();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            norm = value.value();
        }
    }
    if (sim_scorer_ == nullptr) {
        return 0.0F;
    }
    return sim_scorer_->score(static_cast<float>(freq_), norm);
}

Result<int32_t> TermAutomatonScorer::scoring_doc_id() { return doc_id_; }

Result<float> TermAutomatonScorer::get_max_score(int32_t /*upto*/) {
    if (sim_scorer_ == nullptr) {
        return 0.0F;
    }
    // The frequency is unbounded, so the bound is the score of an arbitrarily
    // frequent term.
    return sim_scorer_->score(std::numeric_limits<float>::max(), 1);
}

TwoPhaseState TermAutomatonScorer::has_two_phase_iterator() const { return TwoPhaseState::No; }

} // namespace lucene
