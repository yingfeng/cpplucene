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

module lucene_core:union_postings.impl;

import :union_postings;

import std;

import :doc_id_set_iterator;
import :error;
import :postings_enum;
import :types;

namespace lucene {

UnionPostingsEnum::UnionPostingsEnum(std::vector<std::unique_ptr<PostingsEnum>> subs) {
    subs_ = std::move(subs);
    heap_.reserve(subs_.size());
    for (size_t i = 0; i < subs_.size(); ++i) {
        heap_.push_back(i);
        auto sub_cost = subs_[i]->cost();
        if (sub_cost.has_value()) {
            cost_ += sub_cost.value();
        }
    }
    std::make_heap(heap_.begin(), heap_.end(),
                   [this](size_t left, size_t right) { return heap_greater(left, right); });
}

size_t UnionPostingsEnum::update_top_index() {
    // The root changed: take it out of the heap and put it straight back, which
    // sifts it down to its new place.
    std::pop_heap(heap_.begin(), heap_.end(),
                  [this](size_t left, size_t right) { return heap_greater(left, right); });
    std::push_heap(heap_.begin(), heap_.end(),
                   [this](size_t left, size_t right) { return heap_greater(left, right); });
    return heap_.front();
}

int32_t UnionPostingsEnum::doc_id() const {
    if (heap_.empty()) {
        return NO_MORE_DOCS;
    }
    return subs_[top_index()]->doc_id();
}

Result<int32_t> UnionPostingsEnum::next_doc() {
    if (heap_.empty()) {
        return NO_MORE_DOCS;
    }
    size_t top = top_index();
    const int32_t doc = subs_[top]->doc_id();
    while (true) {
        auto advanced = subs_[top]->next_doc();
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        top = update_top_index();
        const int32_t next = subs_[top]->doc_id();
        if (next != doc) {
            return next;
        }
    }
}

Result<int32_t> UnionPostingsEnum::advance(int32_t target) {
    if (heap_.empty()) {
        return NO_MORE_DOCS;
    }
    size_t top = top_index();
    while (true) {
        auto advanced = subs_[top]->advance(target);
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        top = update_top_index();
        const int32_t doc = subs_[top]->doc_id();
        if (doc >= target) {
            return doc;
        }
    }
}

Result<int32_t> UnionPostingsEnum::freq() {
    const int32_t doc = doc_id();
    if (doc != positions_doc_) {
        positions_.clear();
        position_index_ = 0;
        for (std::unique_ptr<PostingsEnum> &sub : subs_) {
            if (sub->doc_id() != doc) {
                continue;
            }
            auto frequency = sub->freq();
            if (!frequency.has_value()) {
                return std::unexpected(std::move(frequency.error()));
            }
            for (int32_t i = 0; i < frequency.value(); ++i) {
                auto position = sub->next_position();
                if (!position.has_value()) {
                    return std::unexpected(std::move(position.error()));
                }
                positions_.push_back(position.value());
            }
        }
        std::ranges::sort(positions_);
        positions_doc_ = doc;
    }
    return static_cast<int32_t>(positions_.size());
}

Result<int32_t> UnionPostingsEnum::next_position() {
    if (position_index_ >= positions_.size()) {
        return err(LuceneError::illegal_state("no more merged positions in this document"));
    }
    return positions_[position_index_++];
}

} // namespace lucene
