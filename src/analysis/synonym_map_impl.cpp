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

module lucene_core:synonym_map.impl;

import :synonym_map;

import :types;

import std;

namespace lucene {

namespace {

/// The separator of the compound key; a control character cannot appear in a
/// token, so two different word sequences never share a key.
constexpr char kSeparator = '\x1F';

} // namespace

std::string SynonymMap::key_of(const std::vector<std::string> &words) {
    std::string key;
    for (size_t i = 0; i < words.size(); ++i) {
        if (i > 0) {
            key.push_back(kSeparator);
        }
        key += words[i];
    }
    return key;
}

void SynonymMap::Builder::add(std::vector<std::string> input, std::vector<std::string> output, bool include_original) {
    if (input.empty()) {
        return;
    }
    std::vector<std::vector<std::string>> alternatives;
    alternatives.push_back(std::move(output));
    if (include_original) {
        alternatives.push_back(input);
    }

    std::vector<std::vector<std::string>> &existing = rules_[SynonymMap::key_of(input)];
    for (std::vector<std::string> &alternative : alternatives) {
        if (deduplicate_ &&
            std::find(existing.begin(), existing.end(), alternative) != existing.end()) {
            continue;
        }
        existing.push_back(std::move(alternative));
    }
}

void SynonymMap::Builder::add_all(
    const std::map<std::vector<std::string>, std::vector<std::vector<std::string>>> &rules) {
    for (const auto &rule : rules) {
        std::vector<std::vector<std::string>> &existing = rules_[SynonymMap::key_of(rule.first)];
        for (const std::vector<std::string> &alternative : rule.second) {
            if (deduplicate_ && std::find(existing.begin(), existing.end(), alternative) != existing.end()) {
                continue;
            }
            existing.push_back(alternative);
        }
        // The input words themselves stay one of the alternatives.
        if (deduplicate_ && std::find(existing.begin(), existing.end(), rule.first) == existing.end()) {
            existing.push_back(rule.first);
        } else if (!deduplicate_) {
            existing.push_back(rule.first);
        }
    }
}

SynonymMap SynonymMap::Builder::build() const {
    SynonymMap map;
    map.rules_ = rules_;
    map.max_input_length_ = 1;
    for (const auto &rule : map.rules_) {
        size_t words = 1;
        for (const char current : rule.first) {
            if (current == kSeparator) {
                ++words;
            }
        }
        map.max_input_length_ = std::max(map.max_input_length_, words);
    }
    return map;
}

const std::vector<std::vector<std::string>> *SynonymMap::lookup(const std::vector<std::string> &input) const {
    const auto found = rules_.find(key_of(input));
    return found != rules_.end() ? &found->second : nullptr;
}

} // namespace lucene
