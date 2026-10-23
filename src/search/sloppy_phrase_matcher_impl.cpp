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

module lucene_core:sloppy_phrase_matcher.impl;

import :sloppy_phrase_matcher;

import std;

import :error;
import :phrase_matcher;
import :phrase_positions;
import :postings_enum;
import :types;

namespace lucene {

SloppyPhraseMatcher::SloppyPhraseMatcher(std::vector<PostingsAndFreq> postings, size_t slop, float match_cost,
                                         bool expose_offsets)
        : PhraseMatcherBase(std::move(postings)), slop_(slop), capture_lead_match_(expose_offsets),
          match_cost_(match_cost) {
    pps_.reserve(num_postings());
    for (size_t i = 0; i < num_postings(); ++i) {
        pps_.emplace_back(i, postings_[i].position, i);
    }
}

// ------------------------------------------------------------- the min heap

bool SloppyPhraseMatcher::less_slot(size_t left, size_t right) const {
    const PhrasePositions &pp1 = pps_[left];
    const PhrasePositions &pp2 = pps_[right];
    if (pp1.position() != pp2.position()) {
        return pp1.position() < pp2.position();
    }
    if (pp1.offset() != pp2.offset()) {
        return pp1.offset() < pp2.offset();
    }
    return pp1.ord() < pp2.ord();
}

void SloppyPhraseMatcher::queue_add(size_t pp_index) {
    queue_.push_back(pp_index);
    // `less_slot` orders ascending, which for the heap means "greater".
    std::push_heap(queue_.begin(), queue_.end(),
                   [this](size_t left, size_t right) { return less_slot(right, left); });
}

size_t SloppyPhraseMatcher::queue_pop() {
    std::pop_heap(queue_.begin(), queue_.end(),
                  [this](size_t left, size_t right) { return less_slot(right, left); });
    const size_t top = queue_.back();
    queue_.pop_back();
    return top;
}

// ------------------------------------------------------------ initialization

int32_t SloppyPhraseMatcher::tp_pos(size_t pp_index) const {
    return pps_[pp_index].position() + pps_[pp_index].offset();
}

VoidResult SloppyPhraseMatcher::place_first_positions() {
    for (size_t i = 0; i < pps_.size(); ++i) {
        auto placed = pps_[i].first_position(postings_at(pps_[i].postings_idx()));
        if (!placed.has_value()) {
            return std::unexpected(std::move(placed.error()));
        }
    }
    return {};
}

VoidResult SloppyPhraseMatcher::fill_queue() {
    queue_.clear();
    for (size_t i = 0; i < pps_.size(); ++i) {
        end_ = std::max(end_, pps_[i].position());
        queue_add(i);
    }
    return {};
}

Result<bool> SloppyPhraseMatcher::advance_repeat_groups() {
    for (const std::vector<size_t> &group : rpt_groups_) {
        // The slots of a group are sorted by offset, so the j-th slot has to
        // start at least j positions after the first one.
        for (size_t j = 1; j < group.size(); ++j) {
            PhrasePositions &pp = pps_[group[j]];
            for (size_t step = 0; step < j; ++step) {
                auto advanced = pp.next_position(postings_at(pp.postings_idx()));
                if (!advanced.has_value()) {
                    return std::unexpected(std::move(advanced.error()));
                }
                if (!advanced.value()) {
                    return false;
                }
            }
        }
    }
    return true;
}

Result<bool> SloppyPhraseMatcher::init_first_time() {
    checked_rpts_ = true;
    auto placed = place_first_positions();
    if (!placed.has_value()) {
        return std::unexpected(std::move(placed.error()));
    }

    // Slots that hold the same term repeat each other.
    std::map<std::string, std::vector<size_t>> by_term;
    for (size_t i = 0; i < postings_.size(); ++i) {
        if (postings_[i].terms.size() == 1) {
            by_term[postings_[i].terms[0].text()].push_back(i);
        }
    }
    for (auto &[text, group] : by_term) {
        if (group.size() < 2) {
            continue;
        }
        std::ranges::sort(group, [this](size_t left, size_t right) {
            return pps_[left].offset() < pps_[right].offset();
        });
        const size_t group_index = rpt_groups_.size();
        for (size_t index = 0; index < group.size(); ++index) {
            pps_[group[index]].set_repetition(group_index, index);
        }
        rpt_groups_.push_back(std::move(group));
    }
    has_rpts_ = !rpt_groups_.empty();

    if (has_rpts_) {
        rpt_stack_.assign(pps_.size(), 0);
        auto advanced = advance_repeat_groups();
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        if (!advanced.value()) {
            return false;
        }
    }

    auto filled = fill_queue();
    if (!filled.has_value()) {
        return std::unexpected(std::move(filled.error()));
    }
    return true;
}

VoidResult SloppyPhraseMatcher::init_simple() {
    queue_.clear();
    for (size_t i = 0; i < pps_.size(); ++i) {
        auto placed = pps_[i].first_position(postings_at(pps_[i].postings_idx()));
        if (!placed.has_value()) {
            return std::unexpected(std::move(placed.error()));
        }
        end_ = std::max(end_, pps_[i].position());
        queue_add(i);
    }
    return {};
}

Result<bool> SloppyPhraseMatcher::init_complex() {
    auto placed = place_first_positions();
    if (!placed.has_value()) {
        return std::unexpected(std::move(placed.error()));
    }
    auto advanced = advance_repeat_groups();
    if (!advanced.has_value()) {
        return std::unexpected(std::move(advanced.error()));
    }
    if (!advanced.value()) {
        return false;
    }
    auto filled = fill_queue();
    if (!filled.has_value()) {
        return std::unexpected(std::move(filled.error()));
    }
    return true;
}

Result<bool> SloppyPhraseMatcher::init_phrase_positions() {
    end_ = std::numeric_limits<int32_t>::min();
    if (!checked_rpts_) {
        return init_first_time();
    }
    if (!has_rpts_) {
        auto placed = init_simple();
        if (!placed.has_value()) {
            return std::unexpected(std::move(placed.error()));
        }
        return true;
    }
    return init_complex();
}

// -------------------------------------------------------------- the matching

Result<bool> SloppyPhraseMatcher::advance_pp(size_t pp_index) {
    PhrasePositions &pp = pps_[pp_index];
    auto advanced = pp.next_position(postings_at(pp.postings_idx()));
    if (!advanced.has_value()) {
        return std::unexpected(std::move(advanced.error()));
    }
    if (!advanced.value()) {
        return false;
    }
    end_ = std::max(end_, pp.position());
    return true;
}

size_t SloppyPhraseMatcher::lesser(size_t left, size_t right) const {
    const PhrasePositions &pp1 = pps_[left];
    const PhrasePositions &pp2 = pps_[right];
    if (pp1.position() < pp2.position() ||
        (pp1.position() == pp2.position() && pp1.offset() < pp2.offset())) {
        return left;
    }
    return right;
}

Result<std::optional<size_t>> SloppyPhraseMatcher::collide(size_t pp_index) {
    const std::optional<size_t> &group = pps_[pp_index].rpt_group();
    if (!group.has_value()) {
        return err(LuceneError::illegal_state("phrase position has no repetition group"));
    }
    const int32_t position = tp_pos(pp_index);
    for (const size_t other : rpt_groups_[group.value()]) {
        if (other != pp_index && tp_pos(other) == position) {
            return std::optional<size_t>(pps_[other].rpt_ind());
        }
    }
    return std::optional<size_t>{};
}

Result<bool> SloppyPhraseMatcher::advance_rpts(size_t pp_index) {
    std::optional<size_t> group = pps_[pp_index].rpt_group();
    if (!group.has_value()) {
        return true;
    }
    rpt_bits_.assign(rpt_groups_[group.value()].size(), false);
    const size_t first_index = pps_[pp_index].rpt_ind();

    auto collision = collide(pp_index);
    if (!collision.has_value()) {
        return std::unexpected(std::move(collision.error()));
    }
    while (collision.value().has_value()) {
        const size_t other = collision.value().value();
        pp_index = lesser(pp_index, rpt_groups_[group.value()][other]);
        auto advanced = advance_pp(pp_index);
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        if (!advanced.value()) {
            return false;
        }
        if (other != first_index) {
            rpt_bits_[other] = true;
        }
        collision = collide(pp_index);
        if (!collision.has_value()) {
            return std::unexpected(std::move(collision.error()));
        }
    }

    // Collisions are resolved; put the slots that were advanced back into the
    // queue, which the collision resolution popped them from.
    size_t remaining = static_cast<size_t>(std::ranges::count(rpt_bits_, true));
    size_t queued = 0;
    while (remaining > 0) {
        const size_t other = queue_pop();
        rpt_stack_[queued++] = other;
        if (pps_[other].rpt_group().has_value()) {
            const size_t index = pps_[other].rpt_ind();
            if (index < rpt_bits_.size() && rpt_bits_[index]) {
                rpt_bits_[index] = false;
                --remaining;
            }
        }
    }
    for (size_t i = queued; i > 0; --i) {
        queue_add(rpt_stack_[i - 1]);
    }
    return true;
}

void SloppyPhraseMatcher::capture_lead(size_t pp_index) {
    if (!capture_lead_match_) {
        return;
    }
    const PhrasePositions &pp = pps_[pp_index];
    lead_ord_ = pp.ord();
    lead_position_ = pp.position() + pp.offset();
    PostingsEnum &postings = postings_at(pp.postings_idx());
    if (auto start = postings.start_offset(); start.has_value()) {
        lead_offset_ = start.value();
    }
    if (auto end = postings.end_offset(); end.has_value()) {
        lead_end_offset_ = end.value();
    }
}

Result<bool> SloppyPhraseMatcher::next_match() {
    if (!positioned_) {
        return false;
    }
    if (queue_.empty()) {
        return err(LuceneError::illegal_state("no phrase positions available"));
    }

    size_t pp_index = queue_pop();
    capture_lead(pp_index);

    match_length_ = end_ - pps_[pp_index].position();
    if (queue_.empty()) {
        return err(LuceneError::illegal_state("no phrase positions available"));
    }
    int32_t next = pps_[queue_top()].position();

    auto advanced = advance_pp(pp_index);
    if (!advanced.has_value()) {
        return std::unexpected(std::move(advanced.error()));
    }
    while (advanced.value()) {
        if (has_rpts_) {
            auto resolved = advance_rpts(pp_index);
            if (!resolved.has_value()) {
                return std::unexpected(std::move(resolved.error()));
            }
            if (!resolved.value()) {
                break; // the positions are exhausted
            }
        }

        const int32_t position = pps_[pp_index].position();
        if (position > next) {
            // The lead passed its successor, so the current width is minimal.
            queue_add(pp_index);
            if (match_length_ <= static_cast<int32_t>(slop_)) {
                return true;
            }
            if (queue_.empty()) {
                return err(LuceneError::illegal_state("no phrase positions available"));
            }
            pp_index = queue_pop();
            if (queue_.empty()) {
                return err(LuceneError::illegal_state("no phrase positions available"));
            }
            next = pps_[queue_top()].position();
            match_length_ = end_ - pps_[pp_index].position();
        } else {
            match_length_ = std::min(match_length_, end_ - position);
        }

        capture_lead(pp_index);
        advanced = advance_pp(pp_index);
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
    }

    positioned_ = false;
    return match_length_ <= static_cast<int32_t>(slop_);
}

VoidResult SloppyPhraseMatcher::reset() {
    auto initialized = init_phrase_positions();
    if (!initialized.has_value()) {
        return std::unexpected(std::move(initialized.error()));
    }
    positioned_ = initialized.value();
    match_length_ = std::numeric_limits<int32_t>::max();
    lead_position_ = std::numeric_limits<int32_t>::max();
    return {};
}

Result<float> SloppyPhraseMatcher::max_freq() {
    if (doc_ < 0 || doc_ == NO_MORE_DOCS) {
        return 0.0F;
    }
    float total = 0.0F;
    for (PostingsAndFreq &entry : postings_) {
        auto frequency = entry.postings->freq();
        if (!frequency.has_value()) {
            return std::unexpected(std::move(frequency.error()));
        }
        total += static_cast<float>(frequency.value());
    }
    return total;
}

float SloppyPhraseMatcher::sloppy_weight() const { return 1.0F / (1.0F + static_cast<float>(match_length_)); }

int32_t SloppyPhraseMatcher::start_position() const {
    int32_t position = lead_position_;
    for (const PhrasePositions &pp : pps_) {
        position = std::min(position, pp.position() + pp.offset());
    }
    return position;
}

int32_t SloppyPhraseMatcher::end_position() const {
    int32_t position = lead_position_;
    for (const PhrasePositions &pp : pps_) {
        if (pp.ord() != lead_ord_) {
            position = std::max(position, pp.position() + pp.offset());
        }
    }
    return position;
}

Result<int32_t> SloppyPhraseMatcher::start_offset() const { return lead_offset_; }

Result<int32_t> SloppyPhraseMatcher::end_offset() const { return lead_end_offset_; }

} // namespace lucene
