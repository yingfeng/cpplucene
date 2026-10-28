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

module lucene_core:wand_scorer.impl;

import :wand_scorer;

import std;

import :bit_util;
import :disi_priority_queue;
import :disi_wrapper;
import :doc_id_set_iterator;
import :error;
import :math_util;
import :scorable;
import :score_mode;
import :scorer;
import :scorer_util;

namespace lucene {

WandScorer::WandScorer(std::vector<std::unique_ptr<Scorer>> sub_scorers, std::vector<DisiWrapper<Scorer *>> wrappers,
                       int32_t min_should_match, ScoreMode score_mode, int64_t lead_cost, int32_t scaling_factor)
        : sub_scorers_(std::move(sub_scorers)),
          wrappers_(std::move(wrappers)),
          score_mode_(score_mode),
          head_(wrappers_.size()),
          lead_cost_(lead_cost),
          tail_(wrappers_.size(), 0),
          min_should_match_(min_should_match),
          scaling_factor_(scaling_factor) {}

Result<std::unique_ptr<WandScorer>> WandScorer::create(std::vector<std::unique_ptr<Scorer>> sub_scorers,
                                                      int32_t min_should_match, ScoreMode score_mode,
                                                      int64_t lead_cost) {
    const size_t num_scorers = sub_scorers.size();
    if (num_scorers < 2) {
        return err(LuceneError::illegal_argument("a WAND scorer needs at least two sub-scorers"));
    }
    if (min_should_match < 1 || static_cast<size_t>(min_should_match) > num_scorers) {
        return err(LuceneError::illegal_argument("min_should_match must be in [1, " + std::to_string(num_scorers) +
                                                 "] (got " + std::to_string(min_should_match) + ")"));
    }
    const bool top_scores = score_mode == ScoreMode::TopScores;

    // The scaling factor preserves the significant bits of the largest score the
    // disjunction can produce, so the integer arithmetic of the pruning does not
    // lose precision.
    int32_t scaling_factor = 0;
    if (top_scores) {
        double max_score_sum = 0;
        for (const std::unique_ptr<Scorer> &sub_scorer : sub_scorers) {
            if (auto shallow = sub_scorer->advance_shallow(0); !shallow.has_value()) {
                return std::unexpected(std::move(shallow.error()));
            }
            auto max_score = sub_scorer->get_max_score(NO_MORE_DOCS);
            if (!max_score.has_value()) {
                return std::unexpected(std::move(max_score.error()));
            }
            max_score_sum += static_cast<double>(max_score.value());
        }
        auto factor = wand_scaling_factor(static_cast<float>(
                MathUtil::sum_upper_bound(max_score_sum, static_cast<int32_t>(num_scorers))));
        if (!factor.has_value()) {
            return std::unexpected(std::move(factor.error()));
        }
        scaling_factor = factor.value();
    }

    std::vector<DisiWrapper<Scorer *>> wrappers;
    std::vector<int64_t> costs;
    wrappers.reserve(num_scorers);
    costs.reserve(num_scorers);
    for (const std::unique_ptr<Scorer> &sub_scorer : sub_scorers) {
        auto cost = sub_scorer->cost();
        if (!cost.has_value()) {
            return std::unexpected(std::move(cost.error()));
        }
        costs.push_back(cost.value());
        wrappers.emplace_back(sub_scorer.get(), cost.value(), sub_scorer->match_cost());
    }

    std::unique_ptr<WandScorer> scorer(new WandScorer(std::move(sub_scorers), std::move(wrappers), min_should_match,
                                                      score_mode, lead_cost, scaling_factor));
    for (size_t idx = 0; idx < num_scorers; ++idx) {
        // Every scorer starts as a lead, in reverse order, like the reference
        // implementation does.
        scorer->add_unpositioned_lead(idx);
    }
    auto cost = ScorerUtil::cost_with_min_should_match(costs, num_scorers, static_cast<size_t>(min_should_match));
    if (!cost.has_value()) {
        return std::unexpected(std::move(cost.error()));
    }
    scorer->cost_ = cost.value();
    return scorer;
}

// ---- lead list ----

VoidResult WandScorer::add_lead(size_t lead_idx) {
    wrappers_[lead_idx].set_next(lead_);
    lead_ = lead_idx;
    ++freq_;
    if (score_mode_ == ScoreMode::TopScores) {
        auto score = scorer_at(lead_idx).score();
        if (!score.has_value()) {
            return std::unexpected(std::move(score.error()));
        }
        lead_score_ += static_cast<double>(score.value());
    }
    return {};
}

void WandScorer::add_unpositioned_lead(size_t lead_idx) {
    wrappers_[lead_idx].set_next(lead_);
    lead_ = lead_idx;
    ++freq_;
}

// ---- head / tail plumbing ----

VoidResult WandScorer::push_back_leads(int32_t target) {
    std::optional<size_t> current = lead_;
    while (current.has_value()) {
        const size_t idx = current.value();
        auto evicted = insert_tail_with_overflow(idx);
        if (evicted.has_value()) {
            const size_t evicted_idx = evicted.value();
            auto new_doc = scorer_at(evicted_idx).advance(target);
            if (!new_doc.has_value()) {
                return std::unexpected(std::move(new_doc.error()));
            }
            wrappers_[evicted_idx].set_doc(new_doc.value());
            (void)head_.add(evicted_idx, wrappers_);
        }
        current = wrappers_[idx].next();
    }
    lead_.reset();
    return {};
}

Result<std::optional<size_t>> WandScorer::advance_head(int32_t target) {
    std::optional<size_t> head_top = head_.top();
    while (head_top.has_value()) {
        const size_t top_idx = head_top.value();
        if (head_doc(top_idx) >= target) {
            break;
        }
        auto evicted = insert_tail_with_overflow(top_idx);
        if (evicted.has_value()) {
            const size_t evicted_idx = evicted.value();
            auto new_doc = scorer_at(evicted_idx).advance(target);
            if (!new_doc.has_value()) {
                return std::unexpected(std::move(new_doc.error()));
            }
            wrappers_[evicted_idx].set_doc(new_doc.value());
            head_top = head_.update_top_with(evicted_idx, wrappers_);
        } else {
            (void)head_.pop(wrappers_);
            head_top = head_.top();
        }
    }
    return head_top;
}

VoidResult WandScorer::advance_tail(size_t idx) {
    auto new_doc = scorer_at(idx).advance(doc_);
    if (!new_doc.has_value()) {
        return std::unexpected(std::move(new_doc.error()));
    }
    wrappers_[idx].set_doc(new_doc.value());
    if (new_doc.value() == doc_) {
        return add_lead(idx);
    }
    (void)head_.add(idx, wrappers_);
    return {};
}

VoidResult WandScorer::advance_tail_top() {
    const size_t top = pop_tail();
    return advance_tail(top);
}

VoidResult WandScorer::advance_all_tail() {
    // Every clause in the tail is advanced, so the heap is simply walked.
    for (size_t i = tail_size_; i-- > 0;) {
        if (auto advanced = advance_tail(tail_[i]); !advanced.has_value()) {
            return advanced;
        }
    }
    tail_size_ = 0;
    tail_max_score_ = 0;
    return {};
}

std::optional<size_t> WandScorer::insert_tail_with_overflow(size_t idx) {
    const int64_t score = wrappers_[idx].scaled_max_score();

    if (tail_max_score_ + score < min_competitive_score_ ||
        tail_size_ + 1 < static_cast<size_t>(min_should_match_)) {
        // There is room for one more entry.
        add_tail(idx);
        tail_max_score_ += score;
        return std::nullopt;
    }
    if (tail_size_ == 0) {
        return idx;
    }
    const size_t top = tail_[0];
    if (!greater_max_score(top, idx)) {
        return idx;
    }
    tail_[0] = idx;
    down_heap_max_score();
    tail_max_score_ = tail_max_score_ - wrappers_[top].scaled_max_score() + score;
    return top;
}

void WandScorer::add_tail(size_t idx) {
    tail_[tail_size_] = idx;
    up_heap_max_score(tail_size_);
    ++tail_size_;
}

size_t WandScorer::pop_tail() {
    const size_t result = tail_[0];
    --tail_size_;
    tail_[0] = tail_[tail_size_];
    down_heap_max_score();
    tail_max_score_ -= wrappers_[result].scaled_max_score();
    return result;
}

bool WandScorer::greater_max_score(size_t left, size_t right) const {
    const int64_t left_score = wrappers_[left].scaled_max_score();
    const int64_t right_score = wrappers_[right].scaled_max_score();
    if (left_score != right_score) {
        return left_score > right_score;
    }
    // Ties go to the cheapest scorer, which is the one likely to advance further.
    return wrappers_[left].cost() < wrappers_[right].cost();
}

void WandScorer::up_heap_max_score(size_t i) {
    const size_t node = tail_[i];
    for (std::optional<size_t> parent = DisiPriorityQueue<Scorer *>::parent_node(i); parent.has_value();
         parent = DisiPriorityQueue<Scorer *>::parent_node(i)) {
        if (!greater_max_score(node, tail_[parent.value()])) {
            break;
        }
        tail_[i] = tail_[parent.value()];
        i = parent.value();
    }
    tail_[i] = node;
}

void WandScorer::down_heap_max_score() {
    const size_t size = tail_size_;
    if (size < 2) {
        return;
    }
    size_t i = 0;
    const size_t node = tail_[0];
    size_t j = DisiPriorityQueue<Scorer *>::left_node(i);
    if (j >= size) {
        return;
    }
    size_t k = DisiPriorityQueue<Scorer *>::right_node(j);
    if (k < size && greater_max_score(tail_[k], tail_[j])) {
        j = k;
    }
    if (!greater_max_score(tail_[j], node)) {
        return;
    }
    for (;;) {
        tail_[i] = tail_[j];
        i = j;
        j = DisiPriorityQueue<Scorer *>::left_node(i);
        k = DisiPriorityQueue<Scorer *>::right_node(j);
        if (k < size && greater_max_score(tail_[k], tail_[j])) {
            j = k;
        }
        if (j >= size || !greater_max_score(tail_[j], node)) {
            break;
        }
    }
    tail_[i] = node;
}

// ---- block boundaries ----

VoidResult WandScorer::update_max_scores(int32_t target) {
    int32_t new_upto = NO_MORE_DOCS;

    // Entries in the head are treated as leads: the least block boundary of a
    // clause whose cost is not above the lead cost becomes the next boundary.
    // The tail is ignored on purpose (it holds the least score contributors) and
    // so are clauses whose cost is above the lead cost, which would otherwise
    // make the boundary recomputation a waste of CPU; when that leaves
    // `NO_MORE_DOCS` this scorer implements plain WAND instead of block-max WAND.
    for (const size_t idx : head_.entries()) {
        if (head_doc(idx) <= new_upto && wrappers_[idx].cost() <= lead_cost_) {
            auto shallow = scorer_at(idx).advance_shallow(head_doc(idx));
            if (!shallow.has_value()) {
                return std::unexpected(std::move(shallow.error()));
            }
            new_upto = std::min(new_upto, shallow.value());
        }
    }

    if (new_upto == NO_MORE_DOCS && tail_size_ > 0 && wrappers_[tail_[0]].cost() <= lead_cost_) {
        const size_t top_idx = tail_[0];
        auto shallow = scorer_at(top_idx).advance_shallow(target);
        if (!shallow.has_value()) {
            return std::unexpected(std::move(shallow.error()));
        }
        new_upto = shallow.value();
        if (const std::optional<size_t> head_top = head_.top(); head_top.has_value()) {
            new_upto = std::max(new_upto, head_doc(head_top.value()));
        }
    }

    upto_ = new_upto;

    for (const size_t idx : head_.entries()) {
        if (head_doc(idx) <= upto_) {
            auto max_score = scorer_at(idx).get_max_score(new_upto);
            if (!max_score.has_value()) {
                return std::unexpected(std::move(max_score.error()));
            }
            wrappers_[idx].set_scaled_max_score(wand_scale_max_score(max_score.value(), scaling_factor_));
        }
    }

    tail_max_score_ = 0;
    for (size_t i = 0; i < tail_size_; ++i) {
        const size_t idx = tail_[i];
        auto shallow = scorer_at(idx).advance_shallow(target);
        if (!shallow.has_value()) {
            return std::unexpected(std::move(shallow.error()));
        }
        auto max_score = scorer_at(idx).get_max_score(upto_);
        if (!max_score.has_value()) {
            return std::unexpected(std::move(max_score.error()));
        }
        wrappers_[idx].set_scaled_max_score(wand_scale_max_score(max_score.value(), scaling_factor_));
        up_heap_max_score(i);
        tail_max_score_ += wrappers_[idx].scaled_max_score();
    }

    // The tail alone must not be able to produce a competitive hit, so entries are
    // moved back to the head while its sum is too large.
    while (tail_size_ > 0 && tail_max_score_ >= min_competitive_score_) {
        const size_t idx = pop_tail();
        auto new_doc = scorer_at(idx).advance(target);
        if (!new_doc.has_value()) {
            return std::unexpected(std::move(new_doc.error()));
        }
        wrappers_[idx].set_doc(new_doc.value());
        (void)head_.add(idx, wrappers_);
    }
    return {};
}

VoidResult WandScorer::move_to_next_block(int32_t target) {
    int32_t current_target = target;
    while (upto_ < NO_MORE_DOCS) {
        const std::optional<size_t> head_top = head_.top();
        if (head_top.has_value()) {
            if (head_doc(head_top.value()) > upto_) {
                if (auto updated = update_max_scores(current_target); !updated.has_value()) {
                    return updated;
                }
                break;
            }
            break;
        }
        // Every clause fits in the tail: their maximum scores cannot reach the
        // minimum competitive score, so the window moves on unchanged.
        current_target = std::max(current_target, upto_ + 1);
        if (auto updated = update_max_scores(current_target); !updated.has_value()) {
            return updated;
        }
    }
    return {};
}

VoidResult WandScorer::move_to_next_candidate() {
    const size_t lead_idx = head_.pop(wrappers_);
    lead_ = lead_idx;
    wrappers_[lead_idx].set_next(std::nullopt);
    freq_ = 1;

    if (score_mode_ == ScoreMode::TopScores) {
        auto score = scorer_at(lead_idx).score();
        if (!score.has_value()) {
            return std::unexpected(std::move(score.error()));
        }
        lead_score_ = static_cast<double>(score.value());
    }

    while (head_.size() > 0) {
        const std::optional<size_t> head_top = head_.top();
        if (!head_top.has_value()) {
            break;
        }
        if (head_doc(head_top.value()) != doc_) {
            break;
        }
        const size_t popped = head_.pop(wrappers_);
        if (auto added = add_lead(popped); !added.has_value()) {
            return added;
        }
    }
    return {};
}

// ---- DocIdSetIterator ----

Result<int32_t> WandScorer::advance(int32_t target) {
    if (auto pushed = push_back_leads(target); !pushed.has_value()) {
        return std::unexpected(std::move(pushed.error()));
    }
    auto head_top = advance_head(target);
    if (!head_top.has_value()) {
        return std::unexpected(std::move(head_top.error()));
    }

    if (score_mode_ == ScoreMode::TopScores &&
        (!head_top.value().has_value() || head_doc(head_top.value().value()) > upto_)) {
        if (auto moved = move_to_next_block(target); !moved.has_value()) {
            return std::unexpected(std::move(moved.error()));
        }
        head_top = head_.top();
    }

    if (head_top.value().has_value()) {
        doc_ = head_doc(head_top.value().value());
    } else {
        doc_ = NO_MORE_DOCS;
    }
    return doc_;
}

// ---- scoring ----

Result<float> WandScorer::score() {
    if (auto advanced = advance_all_tail(); !advanced.has_value()) {
        return std::unexpected(std::move(advanced.error()));
    }
    double lead_score = lead_score_;
    if (score_mode_ != ScoreMode::TopScores) {
        // With top scores the lead score was accumulated on the way in.
        std::optional<size_t> current = lead_;
        while (current.has_value()) {
            const size_t idx = current.value();
            auto sub_score = scorer_at(idx).score();
            if (!sub_score.has_value()) {
                return std::unexpected(std::move(sub_score.error()));
            }
            lead_score += static_cast<double>(sub_score.value());
            current = wrappers_[idx].next();
        }
    }
    return static_cast<float>(lead_score);
}

Result<float> WandScorer::get_max_score(int32_t upto) {
    double sum = 0;
    for (size_t idx = 0; idx < wrappers_.size(); ++idx) {
        auto max_score = scorer_at(idx).get_max_score(upto);
        if (!max_score.has_value()) {
            return std::unexpected(std::move(max_score.error()));
        }
        sum += static_cast<double>(max_score.value());
    }
    return static_cast<float>(sum);
}

VoidResult WandScorer::set_min_competitive_score(float min_score) {
    if (score_mode_ != ScoreMode::TopScores) {
        return err(LuceneError::illegal_state("a minimum competitive score is only supported with top scores"));
    }
    if (min_score < 0.0F) {
        return err(LuceneError::illegal_argument("the minimum competitive score must not be negative"));
    }
    const int64_t scaled = wand_scale_min_score(min_score, scaling_factor_);
    // The collector only ever raises the bar.
    min_competitive_score_ = std::max(min_competitive_score_, scaled);
    return {};
}

Result<bool> WandScorer::matches() {
    if (auto moved = move_to_next_candidate(); !moved.has_value()) {
        return std::unexpected(std::move(moved.error()));
    }

    int64_t scaled_lead_score = 0;
    if (score_mode_ == ScoreMode::TopScores) {
        scaled_lead_score = wand_scale_max_score(
                static_cast<float>(MathUtil::sum_upper_bound(lead_score_, FLOAT_MANTISSA_BITS)), scaling_factor_);
    }

    while (scaled_lead_score < min_competitive_score_ || freq_ < min_should_match_) {
        if (scaled_lead_score + tail_max_score_ < min_competitive_score_ ||
            freq_ + static_cast<int32_t>(tail_size_) < min_should_match_) {
            return false;
        }
        // A match on this document is still possible: advance the most promising
        // clause of the tail.
        const std::optional<size_t> previous_lead = lead_;
        if (auto advanced = advance_tail_top(); !advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        if (score_mode_ == ScoreMode::TopScores && lead_ != previous_lead) {
            scaled_lead_score = wand_scale_max_score(
                    static_cast<float>(MathUtil::sum_upper_bound(lead_score_, FLOAT_MANTISSA_BITS)), scaling_factor_);
        }
    }
    return true;
}

// ---- scaling helpers ----

Result<int32_t> wand_scaling_factor(float value) {
    if (value < 0.0F) {
        return err(LuceneError::illegal_argument("scores must be positive or null"));
    }
    if (value == 0.0F) {
        auto factor = wand_scaling_factor(BitUtil::F32_MIN_VALUE);
        if (!factor.has_value()) {
            return std::unexpected(std::move(factor.error()));
        }
        return factor.value() + 1;
    }
    if (std::isinf(value)) {
        auto factor = wand_scaling_factor(std::numeric_limits<float>::max());
        if (!factor.has_value()) {
            return std::unexpected(std::move(factor.error()));
        }
        return factor.value() - 1;
    }
    // Widening to double gives even subnormal floats a normal exponent.
    const double widened = static_cast<double>(value);
    const auto bits = std::bit_cast<uint64_t>(widened);
    const int32_t exponent = static_cast<int32_t>((bits >> 52) & 0x7FFU) - 1023;
    return FLOAT_MANTISSA_BITS - 1 - exponent;
}

int64_t wand_scale_max_score(float max_score, int32_t scaling_factor) {
    const double scaled = static_cast<double>(max_score) * std::pow(2.0, static_cast<double>(scaling_factor));
    if (scaled > static_cast<double>(MAX_SCALED_SCORE)) {
        // A scorer that answers +infinity, or one whose local maximum is above the
        // global one, lands here.
        return MAX_SCALED_SCORE;
    }
    return static_cast<int64_t>(std::ceil(scaled));
}

int64_t wand_scale_min_score(float min_score, int32_t scaling_factor) {
    const double scaled = static_cast<double>(min_score) * std::pow(2.0, static_cast<double>(scaling_factor));
    return static_cast<int64_t>(std::floor(scaled));
}

} // namespace lucene
