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

export module lucene_core:identity;

import :types;
import std;

namespace lucene {

/// A lightweight identity token: two values compare equal only when one was
/// copied from the other.
///
/// Port of `Identity` (`core/index/index_reader.rs`).  rlucene wraps an
/// `Arc<IdentityTag>` and compares the allocation address; here the tag is a
/// `std::shared_ptr` to an empty object and the comparison is the pointer
/// comparison `std::shared_ptr` already implements.  Following rlucene, a
/// default constructed value is a *fresh* identity, not a shared "null" one.
///
/// Used for "same instance" checks that must not depend on value equality:
/// whether two queries share a term state, whether a `TermStates` was built for
/// the reader a `Weight` is now used with, and so on.
export class Identity {
public:
    Identity() : tag_(std::make_shared<const Tag>()) {}

    [[nodiscard]] bool operator==(const Identity &other) const noexcept { return tag_ == other.tag_; }

    /// Whether this token was copied from `other` (a cheap identity test, kept
    /// for call sites that read better than `==`).
    [[nodiscard]] bool is_same(const Identity &other) const noexcept { return tag_ == other.tag_; }

    /// Hash of the underlying allocation, for use in `std::unordered_map`.
    [[nodiscard]] size_t hash() const noexcept { return std::hash<const Tag *>{}(tag_.get()); }

    /// The raw identity, used in diagnostics only.
    [[nodiscard]] std::string to_string() const {
        return "Identity(0x" + std::format("{:x}", reinterpret_cast<std::uintptr_t>(tag_.get())) + ")";
    }

private:
    struct Tag final {};

    std::shared_ptr<const Tag> tag_;
};

} // namespace lucene
