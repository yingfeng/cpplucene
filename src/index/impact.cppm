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

export module lucene_core:impact;

import :types;
import std;

namespace lucene {

/// Per-document scoring factors.  Port of `Impact` (`core/index/impact.rs`).
///
/// The ordering is the one the reference implementation derives manually: increasing `freq` first,
/// then **decreasing unsigned** `norm` — i.e. the most competitive pairs come
/// first when iterating ascending.
export struct Impact {
    /// Term frequency of the term in the document.
    int32_t freq{0};
    /// Norm factor of the document.
    int64_t norm{0};

    friend bool operator==(const Impact &lhs, const Impact &rhs) = default;

    friend bool operator<(const Impact &lhs, const Impact &rhs) noexcept {
        if (lhs.freq != rhs.freq) {
            return lhs.freq < rhs.freq;
        }
        return static_cast<uint64_t>(rhs.norm) < static_cast<uint64_t>(lhs.norm);
    }
};

/// `{freq=...,norm=...}`, the `Display` implementation of the reference implementation.
[[nodiscard]] inline std::string to_string(const Impact &impact) {
    return std::format("{{freq={},norm={}}}", impact.freq, impact.norm);
}

} // namespace lucene
