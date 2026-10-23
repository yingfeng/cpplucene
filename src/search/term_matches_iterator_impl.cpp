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

module lucene_core:term_matches_iterator.impl;

import :term_matches_iterator;

import std;

import :error;
import :matches_iterator;
import :postings_enum;
import :query;
import :types;

namespace lucene {

TermMatchesIterator::TermMatchesIterator(std::unique_ptr<PostingsEnum> postings, std::shared_ptr<Query> query,
                                        int32_t upto)
        : upto_(upto), postings_(std::move(postings)), query_(std::move(query)) {}

Result<std::unique_ptr<TermMatchesIterator>> TermMatchesIterator::create(std::unique_ptr<PostingsEnum> postings,
                                                                        std::shared_ptr<Query> query) {
    if (postings == nullptr) {
        return err(LuceneError::illegal_argument("a term matches iterator needs postings"));
    }
    auto frequency = postings->freq();
    if (!frequency.has_value()) {
        return std::unexpected(std::move(frequency.error()));
    }
    std::unique_ptr<TermMatchesIterator> iterator(
            new TermMatchesIterator(std::move(postings), std::move(query), frequency.value()));
    return iterator;
}

Result<bool> TermMatchesIterator::next() {
    const int32_t previous = upto_;
    --upto_;
    if (previous <= 0) {
        return false;
    }
    auto position = postings_->next_position();
    if (!position.has_value()) {
        return std::unexpected(std::move(position.error()));
    }
    pos_ = position.value();
    return true;
}

} // namespace lucene
