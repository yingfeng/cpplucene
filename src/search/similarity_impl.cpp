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

module lucene_core:similarity.impl;

import :similarity;

import :collection_statistics;
import :error;
import :explanation;
import :field_invert_state;
import :index_types;
import :number;
import :small_float;
import :term_statistics;
import :types;

import std;

namespace lucene {

Result<Explanation> SimScorer::explain(const Explanation &freq, int64_t norm) const {
    const float freq_value = static_cast<float>(number_as_f64(freq.value()));
    const float value = score(freq_value, norm);
    const std::string description = "score(freq=" + number_to_string(freq.value()) + "), with freq of:";
    return Explanation::match_no_details(value, description);
}

Result<int64_t> Similarity::compute_norm(const FieldInvertState &state) const {
    int32_t num_terms = 0;
    if (state.index_options() == IndexOptions::Docs) {
        num_terms = state.unique_term_count();
    } else if (get_discount_overlaps()) {
        num_terms = state.length() - state.num_overlap();
    } else {
        num_terms = state.length();
    }
    auto encoded = SmallFloat::int_to_byte4(num_terms);
    if (!encoded.has_value()) {
        return std::unexpected(std::move(encoded.error()));
    }
    // Sign extend the norm byte to int64, as Java does.
    return static_cast<int64_t>(static_cast<int8_t>(encoded.value()));
}

} // namespace lucene
