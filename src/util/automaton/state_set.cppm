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

export module lucene_core:state_set;

import :types;
import std;

import :bit_mixer;
import :error;

namespace lucene {

/// A set of automaton states with reference counts; a state is dropped when its
/// count reaches zero.
///
/// Port of `StateSet` (`core/util/automation/state_set.rs`), which is the
/// `IntSet` implementation used by determinization.
///
/// **Deviations:** the reference implementation has two flavours (a shared `Arc<Vec<i32>>` cache and
/// an exclusively owned one); C++ needs only the owned one, so there is a single
/// type.  `long_hash_code` is public here because determinization (not a test)
/// needs it.
export class StateSet {
public:
    explicit StateSet(size_t capacity = 5);

    /// Adds `state`, increasing its reference count by one.
    void incr(int32_t state);
    /// Decreases the reference count of `state`; fails when it is absent.
    [[nodiscard]] VoidResult decr(int32_t state);
    /// Removes every state.
    void reset();

    [[nodiscard]] size_t size() const noexcept { return inner_.size(); }

    /// A sorted snapshot of the states, valid until the next mutation.
    [[nodiscard]] const std::vector<int32_t> &get_array();
    /// The hash of the current states; order independent.
    [[nodiscard]] int64_t long_hash_code();

private:
    void key_changed() {
        hash_updated_ = false;
        array_updated_ = false;
    }

    std::unordered_map<int32_t, int32_t> inner_;
    int64_t hash_code_{0};
    bool hash_updated_{true};
    bool array_updated_{true};
    std::vector<int32_t> array_cache_;
};

} // namespace lucene
