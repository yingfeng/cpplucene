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

module lucene_core:phrase_impacts.impl;

import :phrase_impacts;

import std;

import :doc_id_set_iterator;
import :error;
import :impact;
import :impacts;
import :impacts_source;
import :types;

namespace lucene {

namespace {

/// The impact of a postings list that carries no information about the
/// upcoming documents; it bounds nothing and is dropped from every merge.
constexpr int32_t kNoImpactFreq = std::numeric_limits<int32_t>::max();
constexpr int64_t kNoImpactNorm = 1;

bool is_no_impact(std::span<const Impact> impacts) {
    return !impacts.empty() && impacts.front().freq == kNoImpactFreq && impacts.front().norm == kNoImpactNorm;
}

/// The smallest level whose impacts are valid up to `upto`, if any; a source
/// that has no such level covers nothing of the merged block and is ignored.
std::optional<size_t> covering_level(const Impacts &impacts, int32_t upto) {
    const size_t levels = impacts.num_levels();
    for (size_t level = 0; level < levels; ++level) {
        if (impacts.get_doc_id_upto(level) >= upto) {
            return std::optional<size_t>(level);
        }
    }
    return std::optional<size_t>{};
}

/// Norms are compared as unsigned, the way the impacts themselves are ordered.
bool norm_greater(int64_t left, int64_t right) {
    return static_cast<uint64_t>(left) > static_cast<uint64_t>(right);
}

/// Merges impact lists (each sorted by increasing frequency) into the
/// `(smallest frequency, largest norm)` pairs that bound all of them.
///
/// The lists are walked in parallel by increasing frequency; whenever the
/// smallest frequency among them changes, the current pair is emitted.  The
/// largest norm seen anywhere is carried over, because a document may hold the
/// smallest frequency together with the longest length, which no single list
/// reports on its own.
void merge_by_frequency(const std::vector<std::span<const Impact>> &lists, std::vector<size_t> &cursor,
                        std::vector<size_t> &live, std::vector<Impact> &merged) {
    merged.clear();
    cursor.assign(lists.size(), 0);
    live.clear();
    live.reserve(lists.size());
    for (size_t i = 0; i < lists.size(); ++i) {
        if (!lists[i].empty()) {
            live.push_back(i);
        }
    }
    if (live.empty()) {
        return;
    }
    if (live.size() == 1) {
        merged.assign(lists[live.front()].begin(), lists[live.front()].end());
        return;
    }

    const auto freq_of = [&lists, &cursor](size_t list) { return lists[list][cursor[list]].freq; };
    const auto norm_of = [&lists, &cursor](size_t list) { return lists[list][cursor[list]].norm; };
    /// Position in `live` of the list with the smallest frequency.
    const auto smallest = [&live, &freq_of]() {
        size_t best = 0;
        for (size_t i = 1; i < live.size(); ++i) {
            if (freq_of(live[i]) < freq_of(live[best])) {
                best = i;
            }
        }
        return best;
    };

    size_t top = smallest();
    int32_t current_freq = freq_of(live[top]);
    int64_t current_norm = 0;
    for (const size_t list : live) {
        if (norm_greater(norm_of(list), current_norm)) {
            current_norm = norm_of(list);
        }
    }

    while (true) {
        if (!merged.empty() && merged.back().norm == current_norm) {
            // Same length as the previous pair, so only the frequency improved.
            merged.back().freq = current_freq;
        } else {
            merged.push_back(Impact{.freq = current_freq, .norm = current_norm});
        }
        bool exhausted = false;
        while (true) {
            const size_t list = live[top];
            ++cursor[list];
            if (cursor[list] >= lists[list].size()) {
                // One term has no more documents below the current norm, so
                // nothing past this point can be competitive either.
                exhausted = true;
                break;
            }
            if (norm_greater(norm_of(list), current_norm)) {
                current_norm = norm_of(list);
            }
            top = smallest();
            if (freq_of(live[top]) != current_freq) {
                break;
            }
        }
        if (exhausted) {
            return;
        }
        current_freq = freq_of(live[top]);
    }
}

/// The per-block impacts of a phrase, merged on demand.
class MergedImpacts final : public Impacts {
public:
    explicit MergedImpacts(size_t lead) : lead_(lead) {}

    /// The child views to merge, refreshed by the owning source before every use.
    [[nodiscard]] std::vector<Impacts *> &children() noexcept { return impacts_; }

    [[nodiscard]] size_t num_levels() const override { return impacts_[lead_]->num_levels(); }

    [[nodiscard]] int32_t get_doc_id_upto(size_t level) const override {
        return impacts_[lead_]->get_doc_id_upto(level);
    }

    [[nodiscard]] Result<std::span<const Impact>> get_impacts(size_t level) const override {
        const int32_t upto = get_doc_id_upto(level);
        lists_.clear();
        for (Impacts *impacts : impacts_) {
            auto other_level = covering_level(*impacts, upto);
            if (!other_level.has_value()) {
                // This term's impacts do not reach the block: ignoring them
                // only makes the bound more conservative.
                continue;
            }
            auto list = impacts->get_impacts(other_level.value());
            if (!list.has_value()) {
                return std::unexpected(std::move(list.error()));
            }
            if (is_no_impact(list.value())) {
                continue;
            }
            lists_.push_back(list.value());
        }
        if (lists_.empty()) {
            merged_.assign(1, Impact{.freq = kNoImpactFreq, .norm = kNoImpactNorm});
        } else {
            merge_by_frequency(lists_, cursor_, live_, merged_);
        }
        return std::span<const Impact>(merged_);
    }

private:
    /// The impacts of every slot, in slot order.
    std::vector<Impacts *> impacts_;
    size_t lead_;
    // Scratch storage, so merging a block does not allocate.
    mutable std::vector<std::span<const Impact>> lists_;
    mutable std::vector<size_t> cursor_;
    mutable std::vector<size_t> live_;
    mutable std::vector<Impact> merged_;
};

} // namespace

PhraseImpacts::PhraseImpacts(std::vector<ImpactsSource *> sources) : sources_(std::move(sources)) {
    // Walk the block boundaries of the cheapest postings, the way
    // `BlockMaxConjunctionScorer` picks the scorer that leads its conjunction.
    int64_t lowest = std::numeric_limits<int64_t>::max();
    for (size_t i = 0; i < sources_.size(); ++i) {
        const auto *iterator = dynamic_cast<const DocIdSetIterator *>(sources_[i]);
        if (iterator == nullptr) {
            continue;
        }
        auto cost = iterator->cost();
        if (!cost.has_value()) {
            continue;
        }
        if (cost.value() < lowest) {
            lowest = cost.value();
            lead_ = i;
        }
    }
    merged_ = std::make_unique<MergedImpacts>(lead_);
}

VoidResult PhraseImpacts::advance_shallow(int32_t target) {
    for (ImpactsSource *source : sources_) {
        if (auto advanced = source->advance_shallow(target); !advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
    }
    return {};
}

Result<Impacts *> PhraseImpacts::get_impacts() {
    auto *merged = static_cast<MergedImpacts *>(merged_.get());
    std::vector<Impacts *> &children = merged->children();
    children.clear();
    children.reserve(sources_.size());
    for (ImpactsSource *source : sources_) {
        auto got = source->get_impacts();
        if (!got.has_value()) {
            return std::unexpected(std::move(got.error()));
        }
        if (got.value() == nullptr) {
            return err(LuceneError::illegal_state("the impacts of a phrase term are missing"));
        }
        children.push_back(got.value());
    }
    return merged_.get();
}

} // namespace lucene
