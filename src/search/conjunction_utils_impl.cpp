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

module lucene_core:conjunction_utils.impl;

import :conjunction_utils;

import :conjunction_disi;
import :conjunction_scorer;
import :doc_id_set_iterator;
import :error;
import :scorer;
import :types;

import std;

namespace lucene {

Result<std::unique_ptr<DocIdSetIterator>>
ConjunctionUtils::intersect_iterators(std::vector<std::unique_ptr<DocIdSetIterator>> iterators) {
    if (iterators.empty()) {
        return std::unique_ptr<DocIdSetIterator>{};
    }
    if (iterators.size() == 1) {
        return std::move(iterators.front());
    }
    auto conjunction = ConjunctionDISI::intersect(std::move(iterators));
    if (!conjunction.has_value()) {
        return std::unexpected(std::move(conjunction.error()));
    }
    return conjunction;
}

Result<std::unique_ptr<Scorer>> ConjunctionUtils::intersect_scorers(std::vector<std::unique_ptr<Scorer>> scorers,
                                                                   std::vector<size_t> scoring) {
    if (scorers.empty()) {
        return std::unique_ptr<Scorer>{};
    }
    if (scorers.size() == 1) {
        // With a single clause there is nothing to intersect: the clause scores
        // itself.
        return std::move(scorers.front());
    }
    return ConjunctionScorer::create(std::move(scorers), std::move(scoring));
}

} // namespace lucene
