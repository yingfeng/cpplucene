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

module lucene_core:parallel_postings_array.impl;

import :parallel_postings_array;

import :array_util;
import :error;
import :types;

import std;

namespace lucene {

PostingsArray::PostingsArray(size_t size)
        : text_starts(size, 0), address_offset(size, 0), byte_starts(size, 0), size_(size) {}

VoidResult ParallelPostingsArray::copy_to(size_t new_size) {
    size_ = new_size;
    if (auto grown = ArrayUtil::grow_exact(text_starts, new_size, [] { return int32_t{0}; }); !grown.has_value()) {
        return grown;
    }
    if (auto grown = ArrayUtil::grow_exact(address_offset, new_size, [] { return int32_t{0}; }); !grown.has_value()) {
        return grown;
    }
    return ArrayUtil::grow_exact(byte_starts, new_size, [] { return int32_t{0}; });
}

} // namespace lucene
