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

export module lucene_core:matches_iterator;

import :types;
import std;

import :error;
import :query;

namespace lucene {

/// An iterator over the match positions (and optionally offsets) of one
/// document and field.
///
/// Port of the `MatchesIterator` trait (`core/search/matches_iterator.rs`).
///
/// Call [`next`] until it answers `false`; the position and offset accessors are
/// only meaningful between two `true` answers.  Matches are ordered by start
/// position and then by end position, and the intervals may overlap.
///
/// **Deviation:** the reference implementation's associated `SubMatches` type
/// collapses into `std::unique_ptr<MatchesIterator>`; the queries that need
/// sub-matches (`PhraseQuery`, `SynonymQuery`, `BooleanQuery`) are not ported
/// yet, and a leaf iterator answers an empty pointer.
export class MatchesIterator {
public:
    virtual ~MatchesIterator() = default;

    /// Moves to the next match; `false` when there is none left.
    [[nodiscard]] virtual Result<bool> next() = 0;

    /// The start position of the current match, `-1` when positions are absent.
    [[nodiscard]] virtual Result<int32_t> start_position() const = 0;

    /// The end position of the current match, `-1` when positions are absent.
    [[nodiscard]] virtual Result<int32_t> end_position() const = 0;

    /// The start offset of the current match, `-1` when offsets are absent.
    [[nodiscard]] virtual Result<int32_t> start_offset() const = 0;

    /// The end offset of the current match, `-1` when offsets are absent.
    [[nodiscard]] virtual Result<int32_t> end_offset() const = 0;

    /// The matches of the individual terms of the current match, or a null
    /// pointer at the leaf level.
    [[nodiscard]] virtual Result<std::unique_ptr<MatchesIterator>> get_sub_matches() = 0;

    /// The query that caused the current match.
    [[nodiscard]] virtual Result<std::shared_ptr<Query>> get_query() const = 0;
};

} // namespace lucene
