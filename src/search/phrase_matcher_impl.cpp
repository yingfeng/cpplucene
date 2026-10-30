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

module lucene_core:phrase_matcher.impl;

import :phrase_matcher;

import std;

import :doc_id_set_iterator;
import :error;
import :impacts_source;
import :phrase_impacts;
import :postings_enum;
import :types;

namespace lucene {

PhraseMatcherBase::PhraseMatcherBase(std::vector<PostingsAndFreq> postings) : postings_(std::move(postings)) {
    // Merging needs the impacts of every slot: with one of them missing the
    // bound would describe only part of the phrase, so skipping is disabled
    // instead of silently loosening (or worse, tightening) it.
    std::vector<ImpactsSource *> sources;
    sources.reserve(postings_.size());
    for (const PostingsAndFreq &entry : postings_) {
        if (entry.impacts == nullptr) {
            return;
        }
        sources.push_back(entry.impacts);
    }
    impacts_ = std::make_unique<PhraseImpacts>(std::move(sources));
}

Result<int64_t> PhraseMatcherBase::cost() const {
    int64_t lowest = std::numeric_limits<int64_t>::max();
    for (const PostingsAndFreq &entry : postings_) {
        auto cost = entry.postings->cost();
        if (!cost.has_value()) {
            return std::unexpected(std::move(cost.error()));
        }
        lowest = std::min(lowest, cost.value());
    }
    return lowest == std::numeric_limits<int64_t>::max() ? int64_t{0} : lowest;
}

} // namespace lucene
