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

module lucene_core:collection_statistics.impl;

import :collection_statistics;

import :error;
import :types;

import std;

namespace lucene {

CollectionStatistics::CollectionStatistics(std::string field, int64_t max_doc, int64_t doc_count,
                                          int64_t sum_total_term_freq, int64_t sum_doc_freq)
    : field_(std::move(field)),
      max_doc_(max_doc),
      doc_count_(doc_count),
      sum_total_term_freq_(sum_total_term_freq),
      sum_doc_freq_(sum_doc_freq) {}

Result<CollectionStatistics> CollectionStatistics::create(std::string field, int64_t max_doc, int64_t doc_count,
                                                          int64_t sum_total_term_freq, int64_t sum_doc_freq) {
    if (max_doc <= 0) {
        return err(LuceneError::illegal_argument("maxDoc must be positive, maxDoc: " + std::to_string(max_doc)));
    }
    if (doc_count <= 0) {
        return err(LuceneError::illegal_argument("docCount must be positive, docCount: " +
                                                 std::to_string(doc_count)));
    }
    if (doc_count > max_doc) {
        return err(LuceneError::illegal_argument("docCount must not exceed maxDoc, docCount: " +
                                                 std::to_string(doc_count) +
                                                 ", maxDoc: " + std::to_string(max_doc)));
    }
    if (sum_doc_freq <= 0) {
        return err(LuceneError::illegal_argument("sumDocFreq must be positive, sumDocFreq: " +
                                                 std::to_string(sum_doc_freq)));
    }
    if (sum_doc_freq < doc_count) {
        return err(LuceneError::illegal_argument("sumDocFreq must be at least docCount, sumDocFreq: " +
                                                 std::to_string(sum_doc_freq) +
                                                 ", docCount: " + std::to_string(doc_count)));
    }
    if (sum_total_term_freq <= 0) {
        return err(LuceneError::illegal_argument("sumTotalTermFreq must be positive, sumTotalTermFreq: " +
                                                 std::to_string(sum_total_term_freq)));
    }
    if (sum_total_term_freq < sum_doc_freq) {
        return err(LuceneError::illegal_argument("sumTotalTermFreq must be at least sumDocFreq, sumTotalTermFreq: " +
                                                 std::to_string(sum_total_term_freq) +
                                                 ", sumDocFreq: " + std::to_string(sum_doc_freq)));
    }
    return CollectionStatistics(std::move(field), max_doc, doc_count, sum_total_term_freq, sum_doc_freq);
}

std::string CollectionStatistics::to_string() const {
    return "CollectionStatistics[field=" + field_ + ", maxDoc=" + std::to_string(max_doc_) +
           ", docCount=" + std::to_string(doc_count_) +
           ", sumTotalTermFreq=" + std::to_string(sum_total_term_freq_) +
           ", sumDocFreq=" + std::to_string(sum_doc_freq_) + "]";
}

} // namespace lucene
