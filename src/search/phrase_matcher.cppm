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

export module lucene_core:phrase_matcher;

import :types;
import std;

import :error;
import :postings_enum;
import :term;

namespace lucene {

/// One phrase slot: the postings of a term (or of several terms, for a
/// multi-term query) plus the slot's position inside the phrase.
///
/// Port of `PostingsAndFreq` (`core/search/phrase_query.rs`).
export struct PostingsAndFreq {
    /// Postings with positions (and offsets when a matches iterator is asked
    /// for).
    std::unique_ptr<PostingsEnum> postings;
    /// Position of the slot inside the phrase.
    int32_t position{0};
    /// The terms of the slot; several only for a multi-term query, and used to
    /// detect repetitions.
    std::vector<Term> terms;
};

/// Finds the matches of a phrase on the documents a conjunction offers.
///
/// Port of the `PhraseMatcher` trait (`core/search/phrase_matcher.rs`).
///
/// **Deviations** (C++ scope): the reference implementation's impacts based
/// approximation and its `TopScores` optimization are not ported: they need an
/// [`ImpactsEnum`](lucene_core:impacts_enum) in [`PostingsAndFreq::postings`]
/// (the port keeps a plain [`PostingsEnum`](lucene_core:postings_enum)) plus a
/// [`SimScorer`](lucene_core:sim_scorer) for the frequency of the whole phrase,
/// and without them only the exact-matcher path could prune.  Consequently the
/// candidate iteration is a plain conjunction of the postings; the trait methods
/// that only exist for `matches()` return the offsets the postings hold, which
/// means the postings must have been opened with the `OFFSETS` flag for them to
/// work.
export class PhraseMatcher {
public:
    virtual ~PhraseMatcher() = default;

    // ---- candidate iteration (the conjunction approximation) ----

    /// The current candidate document, `-1` before the first advance and
    /// [`NO_MORE_DOCS`] when exhausted.
    [[nodiscard]] virtual int32_t doc_id() const = 0;
    [[nodiscard]] virtual Result<int32_t> next_doc() = 0;
    [[nodiscard]] virtual Result<int32_t> advance(int32_t target) = 0;
    [[nodiscard]] virtual Result<int64_t> cost() const = 0;

    // ---- matching ----

    /// An upper bound on the number of matches of the current document.
    [[nodiscard]] virtual Result<float> max_freq() = 0;

    /// Prepares the current candidate document; called after the candidate
    /// cursor moved.
    [[nodiscard]] virtual VoidResult reset() = 0;

    /// The next match of the current document; `false` when there is none left.
    [[nodiscard]] virtual Result<bool> next_match() = 0;

    /// The slop adjusted weight of the current match; the sum of these is the
    /// frequency the document is scored with.
    [[nodiscard]] virtual float sloppy_weight() const = 0;

    /// The start position of the current match.
    [[nodiscard]] virtual int32_t start_position() const = 0;
    /// The end position of the current match.
    [[nodiscard]] virtual int32_t end_position() const = 0;
    /// The start offset of the current match; needs the `OFFSETS` flag.
    [[nodiscard]] virtual Result<int32_t> start_offset() const = 0;
    /// The end offset of the current match; needs the `OFFSETS` flag.
    [[nodiscard]] virtual Result<int32_t> end_offset() const = 0;

    /// An estimate of the cost of finding every match of a document.
    [[nodiscard]] virtual float get_match_cost() const = 0;
};

/// A [`PhraseMatcher`] whose candidates are the documents all of its postings
/// agree on.
///
/// **Deviation:** the reference implementation builds this conjunction out of
/// `ConjunctionDISI` and impacts enums; here it is a plain loop over the
/// postings, which is what the port's non impacts scorers do as well.
export class PhraseMatcherBase : public PhraseMatcher {
public:
    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override;

protected:
    explicit PhraseMatcherBase(std::vector<PostingsAndFreq> postings);

    [[nodiscard]] PostingsEnum &postings_at(size_t index) { return *postings_[index].postings; }
    [[nodiscard]] const std::vector<PostingsAndFreq> &postings() const noexcept { return postings_; }
    [[nodiscard]] size_t num_postings() const noexcept { return postings_.size(); }

    /// Moves `doc_` to the first document at or after `target` that all the
    /// postings agree on.
    [[nodiscard]] Result<int32_t> align(int32_t target);

    std::vector<PostingsAndFreq> postings_;
    int32_t doc_{-1};
};

} // namespace lucene
