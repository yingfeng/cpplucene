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

export module lucene_core:conjunction_utils;

import :types;

import std;

import :conjunction_disi;
import :conjunction_scorer;
import :doc_id_set_iterator;
import :error;
import :scorer;

namespace lucene {

/// Helpers that turn a list of sub-iterators into a single conjunction.
///
/// Port of `ConjunctionUtils` (`core/search/conjunction_utils.rs`).
///
/// rlucene keeps the "how many iterators do I have" bookkeeping in its boolean
/// scorer supplier; the port collects it here because the C++ type system cannot
/// collapse a one-element conjunction into its element.
export class ConjunctionUtils {
public:
    /// The conjunction of `iterators`.
    ///
    /// Returns `nullptr` for an empty input (which matches nothing), the single
    /// iterator itself when there is only one, and a [`ConjunctionDISI`] beyond
    /// that.
    [[nodiscard]] static Result<std::unique_ptr<DocIdSetIterator>>
    intersect_iterators(std::vector<std::unique_ptr<DocIdSetIterator>> iterators);

    /// The conjunction of `scorers`, scoring with the `scoring` sub-scorers.
    ///
    /// Returns `nullptr` for an empty input and the single scorer itself when
    /// there is only one; a [`ConjunctionScorer`] otherwise.
    [[nodiscard]] static Result<std::unique_ptr<Scorer>> intersect_scorers(
            std::vector<std::unique_ptr<Scorer>> scorers, std::vector<size_t> scoring);
};

} // namespace lucene
