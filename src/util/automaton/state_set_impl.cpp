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

module lucene_core:state_set.impl;

import :state_set;

import :bit_mixer;
import :error;
import :types;

import std;

namespace lucene {

StateSet::StateSet(size_t capacity) { inner_.reserve(capacity); }

void StateSet::incr(int32_t state) {
    auto found = inner_.find(state);
    if (found == inner_.end()) {
        inner_.emplace(state, 1);
        key_changed();
        return;
    }
    found->second += 1;
}

VoidResult StateSet::decr(int32_t state) {
    auto found = inner_.find(state);
    if (found == inner_.end()) {
        return err(LuceneError::illegal_state(std::format("state {} not found", state)));
    }
    found->second -= 1;
    if (found->second == 0) {
        inner_.erase(found);
        key_changed();
    }
    return {};
}

void StateSet::reset() {
    inner_.clear();
    key_changed();
}

const std::vector<int32_t> &StateSet::get_array() {
    if (!array_updated_) {
        array_cache_.clear();
        array_cache_.reserve(inner_.size());
        for (const auto &entry : inner_) {
            array_cache_.push_back(entry.first);
        }
        std::sort(array_cache_.begin(), array_cache_.end());
        array_updated_ = true;
    }
    return array_cache_;
}

int64_t StateSet::long_hash_code() {
    if (hash_updated_) {
        return hash_code_;
    }
    auto hash = static_cast<int64_t>(inner_.size());
    for (const auto &entry : inner_) {
        hash += static_cast<int64_t>(BitMixer::mix32(static_cast<uint32_t>(entry.first)));
    }
    hash_code_ = hash;
    hash_updated_ = true;
    return hash_code_;
}

} // namespace lucene
