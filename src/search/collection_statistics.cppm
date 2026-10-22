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

export module lucene_core:collection_statistics;

import :types;
import std;

import :error;

namespace lucene {

/// Collection-wide statistics of one field, the input of a [`Similarity`].
///
/// Port of `CollectionStatistics` (`core/search/collection_statistics.rs`).
///
/// * `max_doc` — documents in the range, whether or not they have the field;
/// * `doc_count` — documents that have at least one term for the field;
/// * `sum_total_term_freq` — tokens of the field ("word count");
/// * `sum_doc_freq` — posting-list entries of the field.
export class CollectionStatistics {
public:
    /// Validates the statistics, mirroring the reference implementation's checks.
    [[nodiscard]] static Result<CollectionStatistics> create(std::string field, int64_t max_doc, int64_t doc_count,
                                                             int64_t sum_total_term_freq, int64_t sum_doc_freq);

    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    [[nodiscard]] int64_t max_doc() const noexcept { return max_doc_; }
    [[nodiscard]] int64_t doc_count() const noexcept { return doc_count_; }
    [[nodiscard]] int64_t sum_total_term_freq() const noexcept { return sum_total_term_freq_; }
    [[nodiscard]] int64_t sum_doc_freq() const noexcept { return sum_doc_freq_; }

    [[nodiscard]] std::string to_string() const;

private:
    CollectionStatistics(std::string field, int64_t max_doc, int64_t doc_count, int64_t sum_total_term_freq,
                         int64_t sum_doc_freq);

    std::string field_;
    int64_t max_doc_{0};
    int64_t doc_count_{0};
    int64_t sum_total_term_freq_{0};
    int64_t sum_doc_freq_{0};
};

} // namespace lucene
