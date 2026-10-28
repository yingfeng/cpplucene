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

export module lucene_core:accountable;

import :types;

import std;

namespace lucene {

/// An object whose RAM usage can be computed.
///
/// Port of `Accountable` (`util/accountable.rs`).  Caches and the writer budget
/// use it to account for the memory they retain.
///
/// **Deviation:** the reference implementation returns `Result<i64>` because its
/// implementations can fail on a poisoned lock or a hash lookup; no C++
/// implementation needs that, so [`ram_bytes_used`] returns `int64_t` directly.
export class Accountable {
public:
    virtual ~Accountable() = default;

    /// Retained memory of this object, in bytes.
    [[nodiscard]] virtual int64_t ram_bytes_used() const = 0;

    /// The nested resources this object owns, as a point-in-time snapshot.
    ///
    /// The pointers are borrowed; implementations must not hand out storage that
    /// does not outlive them.
    [[nodiscard]] virtual std::vector<const Accountable *> get_child_resources() const { return {}; }

    /// [`ram_bytes_used`] plus the bytes of every child resource.
    [[nodiscard]] int64_t ram_bytes_used_deep() const {
        int64_t total = ram_bytes_used();
        for (const Accountable *child : get_child_resources()) {
            if (child != nullptr) {
                total += child->ram_bytes_used_deep();
            }
        }
        return total;
    }
};

} // namespace lucene
