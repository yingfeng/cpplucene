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

export module lucene_core:matches_utils;

import :types;
import std;

import :error;
import :matches;
import :matches_iterator;

namespace lucene {

/// The [`Matches`] of a query that has no position information at all, for
/// example a point or doc values query: the document matches, but every field
/// answers a null iterator.
///
/// Port of `MatchWithNoTerms` (`core/search/matches_utils.rs`).
export class MatchWithNoTerms final : public Matches {
public:
    [[nodiscard]] Result<std::unique_ptr<MatchesIterator>> get_matches(std::string_view /*field*/) const override {
        return std::unique_ptr<MatchesIterator>{};
    }

    [[nodiscard]] std::vector<std::string> fields() const override { return {}; }
};

/// A fresh instance of [`MatchWithNoTerms`].
///
/// **Deviation:** the reference implementation shares one static instance; the
/// port hands out a new one, which is cheap and avoids a global.
[[nodiscard]] std::unique_ptr<Matches> match_with_no_terms();

/// The [`Matches`] of a query whose sub-queries each have their own matches: the
/// iterator of a field is the disjunction of the sub-iterators of that field.
///
/// Port of `CombinedMatch` (`core/search/matches_utils.rs`).
export class CombinedMatch final : public Matches {
public:
    explicit CombinedMatch(std::vector<std::unique_ptr<Matches>> sub);

    [[nodiscard]] Result<std::unique_ptr<MatchesIterator>> get_matches(std::string_view field) const override;

    /// The union of the fields of the sub-matches, in first-seen order.
    [[nodiscard]] std::vector<std::string> fields() const override;

    /// The sub-matches this instance combines.
    [[nodiscard]] const std::vector<std::unique_ptr<Matches>> &sub_matches() const noexcept { return sub_; }

private:
    std::vector<std::unique_ptr<Matches>> sub_;
};

/// Collapses the sub-matches of a composite query the way the reference
/// implementation's `from_sub_matches` does: nothing is a null pointer, a single
/// match with terms is that match, and anything else is a [`CombinedMatch`].
[[nodiscard]] std::unique_ptr<Matches> from_sub_matches(std::vector<std::unique_ptr<Matches>> sub_matches);

/// The [`Matches`] of a query that only knows a single field.
///
/// Port of `FieldMatches` (`core/search/matches_utils.rs`).  The supplier builds
/// the iterator on demand; the first one is handed out eagerly by [`for_field`]
/// and cached here, so a second call to [`get_matches`] rebuilds it.
///
/// **Deviation:** the reference implementation's `FieldMatchesSupplier` is a
/// typed node over every weight that can report matches; the port takes a
/// `std::function`, because [`Matches`] is already type-erased.
export class FieldMatches final : public Matches {
public:
    FieldMatches(std::string field, std::function<Result<std::unique_ptr<MatchesIterator>>()> supplier,
                 std::unique_ptr<MatchesIterator> first);

    [[nodiscard]] Result<std::unique_ptr<MatchesIterator>> get_matches(std::string_view field) const override;

    [[nodiscard]] std::vector<std::string> fields() const override;

private:
    std::string field_;
    std::function<Result<std::unique_ptr<MatchesIterator>>()> supplier_;
    /// The iterator built by [`for_field`], handed out by the first call.
    mutable std::unique_ptr<MatchesIterator> cached_;
};

/// The [`Matches`] of a query that can only match `field`, or a null pointer when
/// the supplier has no iterator.
[[nodiscard]] Result<std::unique_ptr<Matches>>
for_field(std::string field, std::function<Result<std::unique_ptr<MatchesIterator>>()> supplier);

} // namespace lucene
