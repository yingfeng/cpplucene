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

module lucene_core:sorted_doc_values.impl;

import :sorted_doc_values;

import :bytes_ref;
import :error;
import :terms_enum;
import :types;

import std;

namespace lucene {

Result<int32_t> SortedDocValues::lookup_term(std::span<const uint8_t> key) {
    auto count = get_value_count();
    if (!count.has_value()) {
        return std::unexpected(std::move(count.error()));
    }
    int32_t low = 0;
    int32_t high = count.value() - 1;

    while (low <= high) {
        const int32_t mid = (low + high) >> 1;
        auto term = lookup_ord(mid);
        if (!term.has_value()) {
            return std::unexpected(std::move(term.error()));
        }
        const std::span<const uint8_t> term_bytes = term.value().as_slice();
        int32_t comparison = 0;
        if (std::lexicographical_compare(term_bytes.begin(), term_bytes.end(), key.begin(), key.end())) {
            comparison = -1;
        } else if (std::lexicographical_compare(key.begin(), key.end(), term_bytes.begin(), term_bytes.end())) {
            comparison = 1;
        }
        if (comparison < 0) {
            low = mid + 1;
        } else if (comparison > 0) {
            high = mid - 1;
        } else {
            return mid;
        }
    }
    return -(low + 1);
}

} // namespace lucene
