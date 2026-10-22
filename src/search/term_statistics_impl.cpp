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

module lucene_core:term_statistics.impl;

import :term_statistics;

import :error;
import :term;
import :types;

import std;

namespace lucene {

TermStatistics::TermStatistics(Term term, int64_t doc_freq, int64_t total_term_freq)
    : term_(std::move(term)), doc_freq_(doc_freq), total_term_freq_(total_term_freq) {}

Result<TermStatistics> TermStatistics::create(Term term, int64_t doc_freq, int64_t total_term_freq) {
    if (doc_freq <= 0) {
        return err(LuceneError::illegal_argument("docFreq must be positive, docFreq: " +
                                                 std::to_string(doc_freq)));
    }
    if (total_term_freq <= 0) {
        return err(LuceneError::illegal_argument("totalTermFreq must be positive, totalTermFreq: " +
                                                 std::to_string(total_term_freq)));
    }
    if (total_term_freq < doc_freq) {
        return err(LuceneError::illegal_argument("totalTermFreq must be at least docFreq, totalTermFreq: " +
                                                 std::to_string(total_term_freq) +
                                                 ", docFreq: " + std::to_string(doc_freq)));
    }
    return TermStatistics(std::move(term), doc_freq, total_term_freq);
}

std::string TermStatistics::to_string() const {
    return "TermStatistics[term=" + term_.to_string() + ", docFreq=" + std::to_string(doc_freq_) +
           ", totalTermFreq=" + std::to_string(total_term_freq_) + "]";
}

} // namespace lucene
