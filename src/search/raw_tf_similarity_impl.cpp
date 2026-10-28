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

module lucene_core:raw_tf_similarity.impl;

import :raw_tf_similarity;

import :collection_statistics;
import :explanation;
import :similarity;
import :term_statistics;
import :types;

import std;

namespace lucene {

Result<std::unique_ptr<SimScorer>> RawTFSimilarity::scorer(float boost,
                                                           const CollectionStatistics &collection_stats,
                                                           std::span<const TermStatistics> term_stats) const {
    static_cast<void>(collection_stats);
    static_cast<void>(term_stats);
    return std::unique_ptr<SimScorer>(new RawTFSimScorer(boost));
}

std::string RawTFSimilarity::name() const {
    return "RawTFSimilarity";
}

float RawTFSimScorer::score(float freq, int64_t norm) const {
    static_cast<void>(norm);
    return boost_ * freq;
}

} // namespace lucene
