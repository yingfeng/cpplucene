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

module lucene_core:constant_score_scorer.impl;

import :constant_score_scorer;

import :doc_id_set_iterator;
import :error;
import :score_mode;
import :scorer;
import :types;

import std;

namespace lucene {

std::unique_ptr<ConstantScoreScorer> ConstantScoreScorer::from_disi(float score, ScoreMode score_mode,
                                                                    std::unique_ptr<DocIdSetIterator> iterator) {
    return std::unique_ptr<ConstantScoreScorer>(new ConstantScoreScorer(score, score_mode, std::move(iterator)));
}

int32_t ConstantScoreScorer::doc_id() const {
    return iterator_->doc_id();
}

Result<int32_t> ConstantScoreScorer::next_doc() {
    return iterator_->next_doc();
}

Result<int32_t> ConstantScoreScorer::advance(int32_t target) {
    return iterator_->advance(target);
}

Result<int32_t> ConstantScoreScorer::slow_advance(int32_t target) {
    return iterator_->slow_advance(target);
}

Result<int64_t> ConstantScoreScorer::cost() const {
    return iterator_->cost();
}

Result<float> ConstantScoreScorer::get_max_score(int32_t /*upto*/) {
    return score_;
}

} // namespace lucene
