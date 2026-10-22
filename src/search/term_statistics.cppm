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

export module lucene_core:term_statistics;

import :types;
import std;

import :error;
import :term;

namespace lucene {

/// Statistics of one term of a field: how many documents contain it and how
/// often it occurs.
///
/// Port of `TermStatistics` (`core/search/term_statistics.rs`).
///
/// * `doc_freq` — documents containing the term, in
///   `[1 .. CollectionStatistics::sum_doc_freq]`;
/// * `total_term_freq` — occurrences of the term, in `[doc_freq .. sum_total_term_freq]`.
export class TermStatistics {
public:
    /// Validates the statistics, mirroring the reference implementation's checks.
    [[nodiscard]] static Result<TermStatistics> create(Term term, int64_t doc_freq, int64_t total_term_freq);

    [[nodiscard]] const Term &term() const noexcept { return term_; }
    [[nodiscard]] int64_t doc_freq() const noexcept { return doc_freq_; }
    [[nodiscard]] int64_t total_term_freq() const noexcept { return total_term_freq_; }

    [[nodiscard]] std::string to_string() const;

private:
    TermStatistics(Term term, int64_t doc_freq, int64_t total_term_freq);

    Term term_;
    int64_t doc_freq_{0};
    int64_t total_term_freq_{0};
};

} // namespace lucene
