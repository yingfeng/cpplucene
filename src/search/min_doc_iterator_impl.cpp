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

module lucene_core:min_doc_iterator.impl;

import :min_doc_iterator;
import :types;

import :doc_id_set_iterator;
import :error;

import std;

namespace lucene {

Result<int32_t> MinDocIterator::advance(int32_t target) {
    if (doc_ == -1) {
        // The first call jumps straight to the segment's smallest document.
        doc_ = std::max(target, segment_min_doc_);
    } else {
        doc_ = target;
    }
    if (doc_ >= max_doc_) {
        doc_ = NO_MORE_DOCS;
    }
    return doc_;
}

} // namespace lucene
