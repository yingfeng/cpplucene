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

module lucene_core:filtered_doc_id_set_iterator.impl;

import :filtered_doc_id_set_iterator;

import std;

import :doc_id_set_iterator;
import :error;
import :types;

namespace lucene {

FilteredDocIdSetIterator::FilteredDocIdSetIterator(std::unique_ptr<DocIdSetIterator> inner)
        : inner_(std::move(inner)) {}

Result<int32_t> FilteredDocIdSetIterator::next_doc() {
    while (true) {
        auto next = inner_->next_doc();
        if (!next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
        doc_ = next.value();
        if (doc_ == NO_MORE_DOCS) {
            return doc_;
        }
        auto accepted = match(doc_);
        if (!accepted.has_value()) {
            return std::unexpected(std::move(accepted.error()));
        }
        if (accepted.value()) {
            return doc_;
        }
    }
}

Result<int32_t> FilteredDocIdSetIterator::advance(int32_t target) {
    auto advanced = inner_->advance(target);
    if (!advanced.has_value()) {
        return std::unexpected(std::move(advanced.error()));
    }
    doc_ = advanced.value();
    if (doc_ == NO_MORE_DOCS) {
        return doc_;
    }
    auto accepted = match(doc_);
    if (!accepted.has_value()) {
        return std::unexpected(std::move(accepted.error()));
    }
    if (accepted.value()) {
        return doc_;
    }
    return next_doc();
}

Result<int64_t> FilteredDocIdSetIterator::cost() const { return inner_->cost(); }

} // namespace lucene
