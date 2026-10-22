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

export module lucene_core:limited_finite_strings_iterator;

import :types;
import std;

import :automaton;
import :error;
import :finite_strings_iterator;
import :ints_ref;

namespace lucene {

/// A [`FiniteStringsIterator`] that limits the number of iterated accepted
/// strings: when more than `limit` strings are accepted, only the first `limit`
/// strings found are returned.
///
/// If the automaton has cycles this iterator may report an error, though that is
/// not guaranteed.
///
/// Be aware that the iteration order is implementation dependent and may change
/// across releases.
///
/// Port of `LimitedFiniteStringsIterator`
/// (`core/util/automation/limited_finite_strings_iterator.rs`).
///
/// **Deviation:** a constructor cannot fail in C++, so construction goes through
/// [`create`]; `next` hands out a copy of the string rather than a borrowed
/// view, like [`FiniteStringsIterator::next`].
export class LimitedFiniteStringsIterator {
public:
    /// An iterator over at most `limit` strings; `-1` means "no limit", other
    /// non-positive limits are rejected.
    [[nodiscard]] static Result<LimitedFiniteStringsIterator> create(const Automaton &a, int32_t limit);

    /// Number of finite strings iterated so far.
    [[nodiscard]] int32_t size() const noexcept { return count_; }

    /// The next accepted string, or `nullopt` once the limit or the automaton is
    /// exhausted.
    [[nodiscard]] Result<std::optional<IntsRef>> next();

private:
    LimitedFiniteStringsIterator(const FiniteStringsIterator &base, int32_t limit);

    int32_t limit_;
    int32_t count_{0};
    FiniteStringsIterator base_;
};

} // namespace lucene
