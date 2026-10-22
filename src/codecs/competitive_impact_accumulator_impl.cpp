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

module lucene_core:competitive_impact_accumulator.impl;

import :competitive_impact_accumulator;

import :types;
import std;

import :debug;
import :impact;

namespace lucene {

void CompetitiveImpactAccumulator::add(int32_t freq, int64_t norm) {
    if (norm >= -128 && norm <= 127) {
        const size_t index = static_cast<size_t>(static_cast<uint8_t>(static_cast<int8_t>(norm)));
        max_freqs_[index] = std::max(max_freqs_[index], freq);
    } else {
        add_entry(Impact{freq, norm}, other_freq_norm_pairs_);
    }
    debug_check(assert_consistent(), "competitive impact accumulator is inconsistent");
}

void CompetitiveImpactAccumulator::add_all(const CompetitiveImpactAccumulator &acc) {
    for (size_t i = 0; i < max_freqs_.size(); ++i) {
        max_freqs_[i] = std::max(max_freqs_[i], acc.max_freqs_[i]);
    }
    for (const Impact &entry : acc.other_freq_norm_pairs_) {
        add_entry(entry, other_freq_norm_pairs_);
    }
    debug_check(assert_consistent(), "competitive impact accumulator is inconsistent");
}

std::vector<Impact> CompetitiveImpactAccumulator::get_competitive_freq_norm_pairs() const {
    std::vector<Impact> impacts;
    int32_t max_freq_for_lower_norms = 0;
    for (size_t i = 0; i < max_freqs_.size(); ++i) {
        const int32_t freq = max_freqs_[i];
        if (freq > max_freq_for_lower_norms) {
            // Byte norms are read back as signed values.
            impacts.push_back(Impact{freq, static_cast<int64_t>(static_cast<int8_t>(static_cast<uint8_t>(i)))});
            max_freq_for_lower_norms = freq;
        }
    }

    if (other_freq_norm_pairs_.empty()) {
        return impacts;
    }

    std::set<Impact> freq_norm_pairs = other_freq_norm_pairs_;
    for (const Impact &impact : impacts) {
        add_entry(impact, freq_norm_pairs);
    }
    return std::vector<Impact>(freq_norm_pairs.begin(), freq_norm_pairs.end());
}

std::string CompetitiveImpactAccumulator::to_string() const {
    std::string result = "[";
    bool first = true;
    for (const Impact &impact : get_competitive_freq_norm_pairs()) {
        if (!first) {
            result.append(", ");
        }
        result.append(lucene::to_string(impact));
        first = false;
    }
    result.push_back(']');
    return result;
}

bool CompetitiveImpactAccumulator::assert_consistent() const {
    int32_t prev_freq = 0;
    uint64_t prev_norm = 0;
    for (const Impact &impact : other_freq_norm_pairs_) {
        debug_check(impact.norm < -128 || impact.norm > 127, "narrow norms belong to max_freqs");
        debug_check(prev_freq < impact.freq, "freq must increase");
        debug_check(prev_norm < static_cast<uint64_t>(impact.norm), "norm must increase (unsigned)");
        prev_freq = impact.freq;
        prev_norm = static_cast<uint64_t>(impact.norm);
    }
    return true;
}

void CompetitiveImpactAccumulator::add_entry(const Impact &new_entry, std::set<Impact> &freq_norm_pairs) {
    auto next = freq_norm_pairs.lower_bound(new_entry);
    if (next != freq_norm_pairs.end() && static_cast<uint64_t>(next->norm) <= static_cast<uint64_t>(new_entry.norm)) {
        // An equivalent or more competitive entry is already there.
        return;
    }

    auto inserted = freq_norm_pairs.insert(new_entry).first;

    // Drop the less competitive entries: lower frequencies with a norm that is
    // not smaller than the new one.
    std::optional<Impact> first_to_remove;
    auto it = inserted;
    while (it != freq_norm_pairs.begin()) {
        --it;
        if (static_cast<uint64_t>(it->norm) >= static_cast<uint64_t>(new_entry.norm)) {
            first_to_remove = *it;
        } else {
            break;
        }
    }
    if (first_to_remove) {
        freq_norm_pairs.erase(freq_norm_pairs.lower_bound(*first_to_remove), inserted);
    }
}

} // namespace lucene
