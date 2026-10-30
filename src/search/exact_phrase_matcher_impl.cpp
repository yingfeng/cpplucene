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

module lucene_core:exact_phrase_matcher.impl;

import :exact_phrase_matcher;

import std;

import :error;
import :phrase_matcher;
import :postings_enum;
import :types;

namespace lucene {

ExactPhraseMatcher::ExactPhraseMatcher(std::vector<PostingsAndFreq> postings, float match_cost)
        : PhraseMatcherBase(std::move(postings)), match_cost_(match_cost) {
    slots_.reserve(num_postings());
    for (const PostingsAndFreq &entry : postings_) {
        slots_.push_back(Slot{.offset = entry.position});
    }
}

Result<bool> ExactPhraseMatcher::advance_position(size_t index, int32_t target) {
    Slot &slot = slots_[index];
    if (slot.pos >= target) {
        return true;
    }
    if (slot.upto == slot.freq) {
        return false;
    }
    while (true) {
        auto position = next_position_at(index);
        if (!position.has_value()) {
            return std::unexpected(std::move(position.error()));
        }
        slot.pos = position.value();
        ++slot.upto;
        if (slot.pos >= target) {
            return true;
        }
        if (slot.upto == slot.freq) {
            return false;
        }
    }
}

Result<float> ExactPhraseMatcher::max_freq() {
    // Load the frequencies eagerly: a scorer asks for this upper bound of the
    // phrase frequency *before* resetting the positions, to decide whether the
    // document is worth matching at all, so the state has to be left in a shape
    // `reset` can finish.
    float lowest = std::numeric_limits<float>::max();
    for (size_t i = 0; i < slots_.size(); ++i) {
        auto frequency = freq_at(i);
        if (!frequency.has_value()) {
            return std::unexpected(std::move(frequency.error()));
        }
        slots_[i].freq = frequency.value();
        lowest = std::min(lowest, static_cast<float>(frequency.value()));
    }
    freqs_loaded_ = true;
    return lowest;
}

VoidResult ExactPhraseMatcher::reset() {
    if (freqs_loaded_) {
        // `max_freq` already read the frequencies of the current document.
        freqs_loaded_ = false;
        for (Slot &slot : slots_) {
            slot.pos = -1;
            slot.upto = 0;
        }
        return {};
    }
    for (size_t i = 0; i < slots_.size(); ++i) {
        auto frequency = freq_at(i);
        if (!frequency.has_value()) {
            return std::unexpected(std::move(frequency.error()));
        }
        slots_[i].freq = frequency.value();
        slots_[i].pos = -1;
        slots_[i].upto = 0;
    }
    return {};
}

Result<bool> ExactPhraseMatcher::next_match() {
    // The lead posting is the one with the fewest positions, so the caller has
    // to hand the postings over sorted by frequency.
    Slot &lead = slots_[0];
    if (lead.upto >= lead.freq) {
        return false;
    }
    auto position = next_position_at(0);
    if (!position.has_value()) {
        return std::unexpected(std::move(position.error()));
    }
    lead.pos = position.value();
    ++lead.upto;

    while (true) {
        const int32_t phrase_pos = lead.pos - lead.offset;
        bool advanced_lead = false;
        for (size_t j = 1; j < slots_.size(); ++j) {
            const int32_t expected = phrase_pos + slots_[j].offset;
            auto reached = advance_position(j, expected);
            if (!reached.has_value()) {
                return std::unexpected(std::move(reached.error()));
            }
            if (!reached.value()) {
                return false;
            }
            if (slots_[j].pos != expected) {
                // The posting went past the position the lead implies, so the
                // lead has to be moved to where this posting is.
                const int32_t target = slots_[j].pos - slots_[j].offset + lead.offset;
                auto jumped = advance_position(0, target);
                if (!jumped.has_value()) {
                    return std::unexpected(std::move(jumped.error()));
                }
                if (!jumped.value()) {
                    return false;
                }
                advanced_lead = true;
                break;
            }
        }
        if (!advanced_lead) {
            return true;
        }
    }
}

int32_t ExactPhraseMatcher::start_position() const { return slots_[0].pos; }

int32_t ExactPhraseMatcher::end_position() const { return slots_[slots_.size() - 1].pos; }

Result<int32_t> ExactPhraseMatcher::start_offset() const {
    // Offsets are only loaded when a matches iterator asks for them.
    PostingsEnum &postings = *postings_[0].postings;
    return postings.start_offset();
}

Result<int32_t> ExactPhraseMatcher::end_offset() const {
    PostingsEnum &postings = *postings_[postings_.size() - 1].postings;
    return postings.end_offset();
}

} // namespace lucene
