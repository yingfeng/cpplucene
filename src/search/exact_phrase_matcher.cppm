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

export module lucene_core:exact_phrase_matcher;

import :types;
import std;

import :error;
import :phrase_matcher;

namespace lucene {

/// Matches a phrase whose terms have to be adjacent, i.e. a query with no slop.
///
/// Port of `ExactPhraseMatcher` (`core/search/exact_phrase_matcher.rs`).
///
/// The first posting leads: its positions are tried in order, and the other
/// postings are advanced to the position the lead implies.
///
/// **Deviation:** the reference implementation sorts the postings by position
/// and wraps them in impacts enums at construction; here the caller supplies
/// the postings in phrase order and the impacts are merged by the base class,
/// which reads them from the postings themselves.
export class ExactPhraseMatcher final : public PhraseMatcherBase {
public:
    ExactPhraseMatcher(std::vector<PostingsAndFreq> postings, float match_cost);

    // ---- PhraseMatcher ----

    /// The frequencies of the current document, loaded into the slots.
    ///
    /// Defined in the interface unit, like [`reset`] and [`next_match`], so that
    /// a scorer holding the concrete matcher inlines the whole per-document
    /// match into its own loop: on a phrase query this is one indirect call per
    /// document per entry point otherwise, and the reference implementation gets
    /// the inlining from monomorphising.
    [[gnu::always_inline]] [[nodiscard]] Result<float> max_freq() override {
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

    [[gnu::always_inline]] [[nodiscard]] VoidResult reset() override {
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

    [[gnu::always_inline]] [[nodiscard]] Result<bool> next_match() override {
        // The lead posting is the one with the fewest positions, so the caller
        // has to hand the postings over sorted by frequency.
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
                    // The posting went past the position the lead implies, so
                    // the lead has to be moved to where this posting is.
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

    [[nodiscard]] float sloppy_weight() const override { return 1.0F; }
    [[nodiscard]] int32_t start_position() const override;
    [[nodiscard]] int32_t end_position() const override;
    [[nodiscard]] Result<int32_t> start_offset() const override;
    [[nodiscard]] Result<int32_t> end_offset() const override;
    [[nodiscard]] float get_match_cost() const override { return match_cost_; }

private:
    /// The position state of one posting.
    struct Slot {
        /// Position inside the phrase.
        int32_t offset{0};
        /// Current position in the document.
        int32_t pos{-1};
        /// Number of positions of the current document.
        int32_t freq{0};
        /// How many positions have been read.
        int32_t upto{0};
    };

    /// Advances the position cursor of `index` to the first position at or after
    /// `target`; `false` when the document has no such position.
    [[gnu::always_inline]] [[nodiscard]] Result<bool> advance_position(size_t index, int32_t target) {
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

    std::vector<Slot> slots_;
    float match_cost_{0.0F};
    /// Whether [`max_freq`] already loaded the frequencies of the current
    /// document, so that [`reset`] only has to rewind the position cursors.
    bool freqs_loaded_{false};
};

} // namespace lucene
