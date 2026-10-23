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

module lucene_core:disjunction_matches_iterator.impl;

import :disjunction_matches_iterator;

import std;

import :error;
import :matches_iterator;
import :priority_queue;
import :query;
import :types;

namespace lucene {

DisjunctionMatchesIterator::DisjunctionMatchesIterator(
        PriorityQueue<std::unique_ptr<MatchesIterator>, DisjunctionMatchesIteratorCompare> queue)
        : queue_(std::move(queue)) {}

Result<std::unique_ptr<MatchesIterator>>
DisjunctionMatchesIterator::from_sub_iterators(std::vector<std::unique_ptr<MatchesIterator>> matches) {
    if (matches.empty()) {
        return std::unique_ptr<MatchesIterator>{};
    }
    if (matches.size() == 1) {
        return std::unique_ptr<MatchesIterator>(std::move(matches.front()));
    }
    auto queue = PriorityQueue<std::unique_ptr<MatchesIterator>, DisjunctionMatchesIteratorCompare>::create(
            matches.size(), DisjunctionMatchesIteratorCompare{});
    if (!queue.has_value()) {
        return std::unexpected(std::move(queue.error()));
    }
    auto built = std::move(queue.value());
    for (auto &sub : matches) {
        auto has_next = sub->next();
        if (!has_next.has_value()) {
            return std::unexpected(std::move(has_next.error()));
        }
        if (!has_next.value()) {
            continue;
        }
        auto added = built.add(std::move(sub));
        if (!added.has_value()) {
            return std::unexpected(std::move(added.error()));
        }
    }
    if (built.size() == 0) {
        return std::unique_ptr<MatchesIterator>{};
    }
    std::unique_ptr<MatchesIterator> result(new DisjunctionMatchesIterator(std::move(built)));
    return result;
}

Result<bool> DisjunctionMatchesIterator::next() {
    if (!started_) {
        started_ = true;
        return queue_.size() > 0;
    }
    auto *top = queue_.top_mut();
    if (top == nullptr) {
        return err(LuceneError::illegal_state("no top available"));
    }
    auto has_next = (*top)->next();
    if (!has_next.has_value()) {
        return std::unexpected(std::move(has_next.error()));
    }
    if (!has_next.value()) {
        auto popped = queue_.pop_unchecked();
        if (!popped.has_value()) {
            return std::unexpected(std::move(popped.error()));
        }
    }
    if (queue_.size() == 0) {
        return false;
    }
    auto updated = queue_.update_top();
    if (!updated.has_value()) {
        return std::unexpected(std::move(updated.error()));
    }
    return true;
}

Result<int32_t> DisjunctionMatchesIterator::start_position() const {
    const auto *top = queue_.top();
    if (top == nullptr) {
        return err(LuceneError::illegal_state("priority queue top element should exist"));
    }
    return (*top)->start_position();
}

Result<int32_t> DisjunctionMatchesIterator::end_position() const {
    const auto *top = queue_.top();
    if (top == nullptr) {
        return err(LuceneError::illegal_state("priority queue top element should exist"));
    }
    return (*top)->end_position();
}

Result<int32_t> DisjunctionMatchesIterator::start_offset() const {
    const auto *top = queue_.top();
    if (top == nullptr) {
        return err(LuceneError::illegal_state("priority queue top element should exist"));
    }
    return (*top)->start_offset();
}

Result<int32_t> DisjunctionMatchesIterator::end_offset() const {
    const auto *top = queue_.top();
    if (top == nullptr) {
        return err(LuceneError::illegal_state("priority queue top element should exist"));
    }
    return (*top)->end_offset();
}

Result<std::unique_ptr<MatchesIterator>> DisjunctionMatchesIterator::get_sub_matches() {
    auto *top = queue_.top_mut();
    if (top == nullptr) {
        return err(LuceneError::illegal_state("no top available"));
    }
    return (*top)->get_sub_matches();
}

Result<std::shared_ptr<Query>> DisjunctionMatchesIterator::get_query() const {
    const auto *top = queue_.top();
    if (top == nullptr) {
        return err(LuceneError::illegal_state("priority queue top element should exist"));
    }
    return (*top)->get_query();
}

} // namespace lucene
