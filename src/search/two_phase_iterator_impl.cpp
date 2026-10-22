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

module lucene_core:two_phase_iterator.impl;

import :two_phase_iterator;

import :doc_id_set_iterator;
import :error;

import std;

namespace lucene {

Result<int32_t> TwoPhaseIterator::approx_slow_advance(int32_t target) {
    int32_t doc = approx_doc_id();
    if (doc == -1) {
        auto next = approx_next_doc();
        if (!next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
        doc = next.value();
    }
    while (doc < target) {
        auto next = approx_next_doc();
        if (!next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
        doc = next.value();
    }
    return doc;
}

TwoPhaseIteratorAsDocIdSetIterator::TwoPhaseIteratorAsDocIdSetIterator(
        std::unique_ptr<TwoPhaseIterator> two_phase)
    : two_phase_(std::move(two_phase)) {}

Result<int32_t> TwoPhaseIteratorAsDocIdSetIterator::do_next(int32_t doc) {
    for (;;) {
        if (doc == NO_MORE_DOCS) {
            return NO_MORE_DOCS;
        }
        auto matches = two_phase_->matches();
        if (!matches.has_value()) {
            return std::unexpected(std::move(matches.error()));
        }
        if (matches.value()) {
            return doc;
        }
        auto next = two_phase_->approx_next_doc();
        if (!next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
        doc = next.value();
    }
}

Result<int32_t> TwoPhaseIteratorAsDocIdSetIterator::next_doc() {
    auto doc = two_phase_->approx_next_doc();
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    return do_next(doc.value());
}

Result<int32_t> TwoPhaseIteratorAsDocIdSetIterator::advance(int32_t target) {
    auto doc = two_phase_->approx_advance(target);
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    return do_next(doc.value());
}

std::unique_ptr<DocIdSetIterator> as_doc_id_set_iterator(std::unique_ptr<TwoPhaseIterator> two_phase) {
    return std::make_unique<TwoPhaseIteratorAsDocIdSetIterator>(std::move(two_phase));
}

} // namespace lucene
