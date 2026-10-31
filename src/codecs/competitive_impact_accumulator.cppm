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

export module lucene_core:competitive_impact_accumulator;

import :types;
import std;

import :impact;

namespace lucene {

/// Accumulates the `(freq, norm)` pairs that may produce competitive scores.
/// Port of `CompetitiveImpactAccumulator`
/// (`core/codecs/competitive_impact_accumulator.rs`).
export class CompetitiveImpactAccumulator {
public:
    CompetitiveImpactAccumulator() { max_freqs_.fill(0); }

    /// Resets to the state it was in right after creation.
    void clear() noexcept {
        max_freqs_.fill(0);
        other_freq_norm_pairs_.clear();
    }

    /// Accumulates a `(freq, norm)` pair, updating this structure if there is
    /// no equivalent or more competitive entry already.
    void add(int32_t freq, int64_t norm);

    /// Merges `acc` into this.
    void add_all(const CompetitiveImpactAccumulator &acc);

    /// Replaces the content of this with `acc`.
    void copy_from(const CompetitiveImpactAccumulator &acc) {
        max_freqs_ = acc.max_freqs_;
        other_freq_norm_pairs_ = acc.other_freq_norm_pairs_;
    }

    /// The set of competitive `(freq, norm)` pairs, ordered by `freq` then by
    /// decreasing unsigned `norm`.
    ///
    /// The pairs are kept in a buffer owned by this accumulator — an inline
    /// array for the common case of at most eight pairs, a vector only when a
    /// term has more — and the view is invalidated by the next call.  Callers
    /// copy them into an output buffer right away (the postings writer does).
    [[nodiscard]] std::span<const Impact> get_competitive_freq_norm_pairs() const;

    /// `[{freq=..,norm=..}, ...]`, the `Display` implementation of the reference implementation.
    [[nodiscard]] std::string to_string() const;

    /// True when the invariants of the accumulator hold (debug builds).
    [[nodiscard]] bool assert_consistent() const;

private:
    static void add_entry(const Impact &new_entry, std::set<Impact> &freq_norm_pairs);

    /// Maps norm values in `-128..127` to the maximum frequency observed for
    /// them.  Norms outside that range go to `other_freq_norm_pairs_`; they are
    /// always absent with the default similarity, which encodes norms as bytes.
    std::array<int32_t, 256> max_freqs_{};
    std::set<Impact> other_freq_norm_pairs_{};

    /// Where [`get_competitive_freq_norm_pairs`] builds its result.
    static constexpr size_t kInlineCapacity = 8;
    mutable std::array<Impact, kInlineCapacity> inline_pairs_{};
    mutable std::vector<Impact> spilled_pairs_{};
};

} // namespace lucene
