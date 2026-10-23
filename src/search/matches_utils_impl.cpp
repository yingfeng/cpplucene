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

module lucene_core:matches_utils.impl;

import :matches_utils;

import std;

import :disjunction_matches_iterator;
import :error;
import :matches;
import :matches_iterator;
import :types;

namespace lucene {

std::unique_ptr<Matches> match_with_no_terms() { return std::make_unique<MatchWithNoTerms>(); }

CombinedMatch::CombinedMatch(std::vector<std::unique_ptr<Matches>> sub) : sub_(std::move(sub)) {}

Result<std::unique_ptr<MatchesIterator>> CombinedMatch::get_matches(std::string_view field) const {
    std::vector<std::unique_ptr<MatchesIterator>> sub_iterators;
    sub_iterators.reserve(sub_.size());
    for (const auto &matches : sub_) {
        auto iterator = matches->get_matches(field);
        if (!iterator.has_value()) {
            return std::unexpected(std::move(iterator.error()));
        }
        if (iterator.value() != nullptr) {
            sub_iterators.push_back(std::move(iterator.value()));
        }
    }
    return DisjunctionMatchesIterator::from_sub_iterators(std::move(sub_iterators));
}

std::vector<std::string> CombinedMatch::fields() const {
    std::vector<std::string> result;
    for (const auto &matches : sub_) {
        for (const auto &field : matches->fields()) {
            if (std::ranges::find(result, field) == result.end()) {
                result.push_back(field);
            }
        }
    }
    return result;
}

std::unique_ptr<Matches> from_sub_matches(std::vector<std::unique_ptr<Matches>> sub_matches) {
    if (sub_matches.empty()) {
        return nullptr;
    }
    size_t match_index = 0;
    size_t match_count = 0;
    for (size_t index = 0; index < sub_matches.size(); ++index) {
        if (dynamic_cast<const MatchWithNoTerms *>(sub_matches[index].get()) == nullptr) {
            match_index = index;
            ++match_count;
        }
    }
    if (match_count == 0) {
        return match_with_no_terms();
    }
    if (match_count == 1) {
        return std::move(sub_matches[match_index]);
    }
    return std::make_unique<CombinedMatch>(std::move(sub_matches));
}

FieldMatches::FieldMatches(std::string field, std::function<Result<std::unique_ptr<MatchesIterator>>()> supplier,
                           std::unique_ptr<MatchesIterator> first)
        : field_(std::move(field)), supplier_(std::move(supplier)), cached_(std::move(first)) {}

Result<std::unique_ptr<MatchesIterator>> FieldMatches::get_matches(std::string_view field) const {
    if (field != field_) {
        return std::unique_ptr<MatchesIterator>{};
    }
    if (cached_ != nullptr) {
        return std::unique_ptr<MatchesIterator>(std::move(cached_));
    }
    return supplier_();
}

std::vector<std::string> FieldMatches::fields() const { return {field_}; }

Result<std::unique_ptr<Matches>>
for_field(std::string field, std::function<Result<std::unique_ptr<MatchesIterator>>()> supplier) {
    auto first = supplier();
    if (!first.has_value()) {
        return std::unexpected(std::move(first.error()));
    }
    if (first.value() == nullptr) {
        return std::unique_ptr<Matches>{};
    }
    std::unique_ptr<Matches> matches(new FieldMatches(std::move(field), std::move(supplier), std::move(first.value())));
    return matches;
}

} // namespace lucene
