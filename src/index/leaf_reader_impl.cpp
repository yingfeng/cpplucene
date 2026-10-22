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

module lucene_core:leaf_reader.impl;

import :leaf_reader;

import :bytes_ref;
import :error;
import :leaf_reader_context;
import :term;
import :terms;
import :terms_enum;
import :types;

import std;

namespace lucene {

Result<int32_t> LeafReader::doc_freq(const Term &term) {
    auto terms = this->terms(term.field());
    if (!terms.has_value()) {
        return std::unexpected(std::move(terms.error()));
    }
    if (terms.value() == nullptr) {
        return 0;
    }
    auto terms_enum = terms.value()->iterator();
    if (!terms_enum.has_value()) {
        return std::unexpected(std::move(terms_enum.error()));
    }
    auto found = terms_enum.value()->seek_exact(term.bytes().as_slice());
    if (!found.has_value()) {
        return std::unexpected(std::move(found.error()));
    }
    if (!found.value()) {
        return 0;
    }
    return terms_enum.value()->doc_freq();
}

Result<int64_t> LeafReader::total_term_freq(const Term &term) {
    auto terms = this->terms(term.field());
    if (!terms.has_value()) {
        return std::unexpected(std::move(terms.error()));
    }
    if (terms.value() == nullptr) {
        return 0;
    }
    auto terms_enum = terms.value()->iterator();
    if (!terms_enum.has_value()) {
        return std::unexpected(std::move(terms_enum.error()));
    }
    auto found = terms_enum.value()->seek_exact(term.bytes().as_slice());
    if (!found.has_value()) {
        return std::unexpected(std::move(found.error()));
    }
    if (!found.value()) {
        return 0;
    }
    return terms_enum.value()->total_term_freq();
}

Result<int64_t> LeafReader::sum_doc_freq(std::string_view field) {
    auto terms = this->terms(field);
    if (!terms.has_value()) {
        return std::unexpected(std::move(terms.error()));
    }
    if (terms.value() == nullptr) {
        return 0;
    }
    return terms.value()->sum_doc_freq();
}

Result<int32_t> LeafReader::doc_count(std::string_view field) {
    auto terms = this->terms(field);
    if (!terms.has_value()) {
        return std::unexpected(std::move(terms.error()));
    }
    if (terms.value() == nullptr) {
        return 0;
    }
    return terms.value()->doc_count();
}

Result<int64_t> LeafReader::sum_total_term_freq(std::string_view field) {
    auto terms = this->terms(field);
    if (!terms.has_value()) {
        return std::unexpected(std::move(terms.error()));
    }
    if (terms.value() == nullptr) {
        return 0;
    }
    return terms.value()->sum_total_term_freq();
}

Result<std::shared_ptr<PointValues>> LeafReader::point_values(std::string_view /*field*/) {
    // The reference implementation leaves this abstract; readers that do not override it report
    // "absent" instead of failing.
    return std::shared_ptr<PointValues>{};
}

Result<std::span<const LeafReaderContext>> LeafReader::leaves() const {
    if (self_leaves_.empty()) {
        auto max = max_doc();
        if (!max.has_value()) {
            return std::unexpected(std::move(max.error()));
        }
        // The context hands out a mutable pointer to the reader: `leaves()` is
        // const only because the array is a cache, the reader itself is not.
        self_leaves_.emplace_back(const_cast<LeafReader *>(this), /*ord=*/0, /*doc_base=*/0,
                                  TopParentMeta{/*leaves_num=*/1, max.value(), identity_});
    }
    return std::span<const LeafReaderContext>(self_leaves_);
}

std::string LeafReader::to_string() const {
    auto max = max_doc();
    auto num = num_docs();
    return "LeafReader(max_doc=" + (max.has_value() ? std::to_string(max.value()) : std::string("<error>")) +
           " num_docs=" + (num.has_value() ? std::to_string(num.value()) : std::string("<error>")) + ")";
}

} // namespace lucene
