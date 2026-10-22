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

module lucene_core:conjunction_disi.impl;

import :conjunction_disi;

import :doc_id_set_iterator;
import :error;
import :types;

import std;

namespace lucene {

Result<std::unique_ptr<ConjunctionDISI>> ConjunctionDISI::create(const std::vector<DocIdSetIterator *> &iterators) {
    if (iterators.size() < 2) {
        return err(LuceneError::illegal_argument("There must be at least 2 sub-iterators, got " +
                                                 std::to_string(iterators.size())));
    }
    for (const auto *iterator : iterators) {
        if (iterator == nullptr) {
            return err(LuceneError::illegal_argument("ConjunctionDISI sub-iterators must not be null"));
        }
    }

    // All sub-iterators have to start on the same document, otherwise the
    // "cheapest two drive the intersection" invariant cannot hold.
    const int32_t cur_doc = iterators[0]->doc_id();
    std::vector<std::pair<int64_t, size_t>> by_cost;
    by_cost.reserve(iterators.size());
    for (size_t i = 0; i < iterators.size(); ++i) {
        if (iterators[i]->doc_id() != cur_doc) {
            return err(LuceneError::illegal_argument(
                    "Sub-iterators of ConjunctionDISI are not on the same document!"));
        }
        auto cost = iterators[i]->cost();
        if (!cost.has_value()) {
            return std::unexpected(std::move(cost.error()));
        }
        by_cost.emplace_back(cost.value(), i);
    }

    // Sort by cost so the two leads are the cheapest; the order of the rest is
    // irrelevant, they are only probed when the leads agree.
    std::ranges::stable_sort(by_cost, [](const auto &left, const auto &right) {
        return left.first < right.first;
    });

    const size_t lead1 = by_cost[0].second;
    const size_t lead2 = by_cost[1].second;
    std::vector<size_t> others;
    others.reserve(by_cost.size() - 2);
    for (size_t i = 2; i < by_cost.size(); ++i) {
        others.push_back(by_cost[i].second);
    }
    return std::unique_ptr<ConjunctionDISI>(new ConjunctionDISI(iterators, lead1, lead2, std::move(others)));
}

Result<std::unique_ptr<DocIdSetIterator>>
ConjunctionDISI::intersect(std::vector<std::unique_ptr<DocIdSetIterator>> iterators) {
    if (iterators.empty()) {
        return std::unique_ptr<DocIdSetIterator>{};
    }
    if (iterators.size() == 1) {
        return std::move(iterators.front());
    }
    std::vector<DocIdSetIterator *> raw;
    raw.reserve(iterators.size());
    for (const auto &iterator : iterators) {
        raw.push_back(iterator.get());
    }
    auto conjunction = create(raw);
    if (!conjunction.has_value()) {
        return std::unexpected(std::move(conjunction.error()));
    }
    // The conjunction keeps the pointers, so it has to keep them alive too.
    conjunction.value()->owned_ = std::move(iterators);
    return std::unique_ptr<DocIdSetIterator>(std::move(conjunction.value()));
}

Result<int32_t> ConjunctionDISI::do_next(int32_t doc) {
    for (;;) {
        // Find agreement between the two cheapest iterators; they are special
        // cased because they cannot be behind `doc` already.
        auto next2 = iterators_[lead2_]->advance(doc);
        if (!next2.has_value()) {
            return next2;
        }
        if (next2.value() != doc) {
            auto advanced = iterators_[lead1_]->advance(next2.value());
            if (!advanced.has_value()) {
                return advanced;
            }
            doc = advanced.value();
            if (doc != next2.value()) {
                if (doc == NO_MORE_DOCS) {
                    return doc;
                }
                continue;
            }
        }
        if (doc == NO_MORE_DOCS) {
            return doc;
        }

        // Then find agreement with the remaining iterators.
        bool restart = false;
        for (const size_t other : others_) {
            const int32_t other_doc = iterators_[other]->doc_id();
            // `other` may already be on `doc` when the previous advance matched
            // exactly.
            if (other_doc < doc) {
                auto next = iterators_[other]->advance(doc);
                if (!next.has_value()) {
                    return next;
                }
                if (next.value() > doc) {
                    // This iterator is beyond `doc`: move the lead forward and
                    // start over from the new highest document.
                    auto advanced = iterators_[lead1_]->advance(next.value());
                    if (!advanced.has_value()) {
                        return advanced;
                    }
                    doc = advanced.value();
                    if (doc == NO_MORE_DOCS) {
                        return doc;
                    }
                    restart = true;
                    break;
                }
            }
        }
        if (!restart) {
            return doc;
        }
    }
}

} // namespace lucene
