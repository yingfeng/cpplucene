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

export module lucene_core:per_field_similarity_wrapper;

import :types;
import std;

import :collection_statistics;
import :field_invert_state;
import :similarity;
import :term_statistics;

namespace lucene {

/// Uses a different [`Similarity`] per field.
///
/// Port of the `PerFieldSimilarityWrapper` trait
/// (`core/search/similarities_impl/per_field_similarity_wrapper.rs`): the
/// defaults of [`compute_norm`] and [`scorer`] pick the similarity with
/// [`get`], which is the only thing a subclass has to implement.
export class PerFieldSimilarityWrapper : public Similarity {
public:
    /// The similarity to use for `field`.
    ///
    /// The returned pointer must stay valid for as long as it is used, which is
    /// what owning the similarities inside the wrapper gives you.
    [[nodiscard]] virtual const Similarity &get(std::string_view field) const = 0;

    /// Delegates to the similarity of `state.name()`.
    [[nodiscard]] Result<int64_t> compute_norm(const FieldInvertState &state) const override;

    /// Delegates to the similarity of `collection_stats.field()`.
    [[nodiscard]] Result<std::unique_ptr<SimScorer>> scorer(
            float boost, const CollectionStatistics &collection_stats,
            std::span<const TermStatistics> term_stats) const override;
};

} // namespace lucene
