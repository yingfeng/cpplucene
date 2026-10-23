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

export module lucene_core:matches;

import :types;
import std;

import :error;
import :matches_iterator;

namespace lucene {

/// The matches of a query in one document, per field.
///
/// Port of the `Matches` trait (`core/search/matches.rs`).
///
/// [`get_matches`] may be called several times, once per field, but not from
/// several threads at once.
///
/// **Deviation:** the reference implementation exposes the sub-`Matches` that
/// make up a composite match; the port does not, because the composite queries
/// (boolean, phrase, synonym) do not implement matches yet.
export class Matches {
public:
    virtual ~Matches() = default;

    /// An iterator over the matches of `field`, or a null pointer when nothing
    /// matched in that field.
    [[nodiscard]] virtual Result<std::unique_ptr<MatchesIterator>> get_matches(std::string_view field) const = 0;

    /// The fields this instance can report matches for.
    [[nodiscard]] virtual std::vector<std::string> fields() const = 0;
};

} // namespace lucene
