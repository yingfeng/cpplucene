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

module lucene_core:docs_with_field_set.impl;

import :docs_with_field_set;

import :bits;
import :doc_id_set;
import :doc_id_set_iterator;
import :error;
import :fixed_bit_set;
import :types;

import std;

namespace lucene {

VoidResult DocsWithFieldSet::add(int32_t doc_id) {
    if (doc_id <= last_doc_id_) {
        return err(LuceneError::illegal_argument("out of order doc ids: last=" + std::to_string(last_doc_id_) +
                                                 ", next=" + std::to_string(doc_id)));
    }
    if (finish_) {
        return err(LuceneError::illegal_state("DocsWithFieldSet must not be changed after finish() is called"));
    }

    const auto doc_index = static_cast<size_t>(doc_id);
    if (set_.has_value()) {
        if (auto grown = set_->ensure_capacity(doc_index); !grown.has_value()) {
            return grown;
        }
        (void)set_->set(doc_index);
    } else if (doc_id != cardinality_) {
        // The first gap: materialize the bits of the contiguous docs added so far.
        FixedBitSet set(doc_index + 1);
        if (auto filled = set.set_with_range(0, static_cast<size_t>(cardinality_)); !filled.has_value()) {
            return filled;
        }
        (void)set.set(doc_index);
        set_.emplace(std::move(set));
    }

    last_doc_id_ = doc_id;
    cardinality_ += 1;
    return {};
}

Result<std::unique_ptr<DocIdSetIterator>> DocsWithFieldSet::iterator() const {
    if (!finish_) {
        return err(LuceneError::illegal_state(
                "DocsWithFieldSet must be call finish() before creating an iterator"));
    }
    if (set_iter_ != nullptr) {
        return std::unique_ptr<DocIdSetIterator>(std::make_unique<FixedBitSetDISI>(set_iter_));
    }
    return std::unique_ptr<DocIdSetIterator>(std::make_unique<AllDISI>(cardinality_));
}

VoidResult DocsWithFieldSet::finish() {
    finish_ = true;
    if (set_.has_value()) {
        set_iter_ = std::make_shared<const FixedBitSet>(std::move(set_.value()));
        set_.reset();
    }
    return {};
}

} // namespace lucene
