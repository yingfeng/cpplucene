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

export module lucene_core:raw_tf_similarity;

import :types;
import std;

import :collection_statistics;
import :explanation;
import :similarity;
import :term_statistics;

namespace lucene {

/// Scores a document with its raw term frequency: `boost * freq`.
///
/// Port of `RawTFSimilarity`
/// (`core/search/similarities_impl/raw_tf_similarity.rs`).  Useful as a
/// reference implementation and in tests, where the score must be predictable.
export class RawTFSimilarity final : public Similarity {
public:
    /// A similarity with the given overlap discounting.
    explicit RawTFSimilarity(bool discount_overlaps) noexcept : discount_overlaps_(discount_overlaps) {}
    /// A similarity that discounts overlaps.
    RawTFSimilarity() noexcept = default;

    [[nodiscard]] bool get_discount_overlaps() const override { return discount_overlaps_; }

    [[nodiscard]] Result<std::unique_ptr<SimScorer>> scorer(
            float boost, const CollectionStatistics &collection_stats,
            std::span<const TermStatistics> term_stats) const override;

    [[nodiscard]] std::string name() const override;

private:
    bool discount_overlaps_{true};
};

/// `boost * freq`.
///
/// Port of `RawTFSimScorer`.
export class RawTFSimScorer final : public SimScorer {
public:
    explicit RawTFSimScorer(float boost) noexcept : boost_(boost) {}

    [[nodiscard]] float score(float freq, int64_t norm) const override;

    [[nodiscard]] float boost() const noexcept { return boost_; }

private:
    float boost_{1.0F};
};

} // namespace lucene
