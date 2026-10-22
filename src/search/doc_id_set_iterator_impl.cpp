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

module lucene_core:doc_id_set_iterator.impl;

import :doc_id_set_iterator;

import :types;
import std;

import :error;
import :debug;

namespace lucene {

Result<int32_t> DocIdSetIterator::advance(int32_t target) {
    (void)target;
    return err(LuceneError::not_implemented(""));
}

Result<int32_t> DocIdSetIterator::slow_advance(int32_t target) {
    debug_check(doc_id() < target, "slow_advance requires target > current doc id");
    int32_t doc = 0;
    do {
        auto next = next_doc();
        if (!next) {
            return next;
        }
        doc = next.value();
    } while (doc < target);
    return doc;
}

Result<int64_t> DocIdSetIterator::cost() const { return err(LuceneError::not_implemented("")); }

Result<int32_t> EmptyDISI::next_doc() {
    debug_check(!exhausted_, "next_doc() called on an exhausted iterator");
    exhausted_ = true;
    return NO_MORE_DOCS;
}

Result<int32_t> EmptyDISI::advance(int32_t target) {
    debug_check(!exhausted_, "advance() called on an exhausted iterator");
    debug_check(target >= 0, "advance() requires a non-negative target");
    exhausted_ = true;
    return NO_MORE_DOCS;
}

Result<int32_t> AllDISI::next_doc() { return advance(doc_ + 1); }

Result<int32_t> AllDISI::advance(int32_t target) {
    doc_ = target;
    if (doc_ >= max_doc_) {
        doc_ = NO_MORE_DOCS;
    }
    return doc_;
}

Result<RangeDISI> RangeDISI::create(int32_t min_doc_id, int32_t max_doc_id) {
    if (min_doc_id >= max_doc_id) {
        return err(LuceneError::illegal_argument(
                std::format("minDoc must be < maxDoc but got minDoc= {} maxDoc= {}", min_doc_id, max_doc_id)));
    }
    if (min_doc_id < 0) {
        return err(LuceneError::illegal_argument(std::format("minDoc must be >= 0 but got minDoc= {}", min_doc_id)));
    }
    return RangeDISI(min_doc_id, max_doc_id);
}

Result<int32_t> RangeDISI::next_doc() { return advance(doc_ + 1); }

Result<int32_t> RangeDISI::advance(int32_t target) {
    if (target < min_doc_) {
        doc_ = min_doc_;
    } else if (target >= max_doc_) {
        doc_ = NO_MORE_DOCS;
    } else {
        doc_ = target;
    }
    return doc_;
}

} // namespace lucene
