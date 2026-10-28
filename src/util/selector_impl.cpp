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

module lucene_core:selector.impl;

import :selector;
import :types;
import :error;

import std;

namespace lucene {

VoidResult Selector::select(size_t /*from*/, size_t /*to*/, size_t /*k*/) {
    return err(LuceneError::need_implemented("select() is not implemented"));
}

VoidResult Selector::swap(size_t /*i*/, size_t /*j*/) {
    return err(LuceneError::need_implemented("swap() is not implemented"));
}

VoidResult Selector::check_args(size_t from, size_t to, size_t k) const {
    if (k < from) {
        return err(LuceneError::illegal_argument("k must be >= from"));
    }
    if (k >= to) {
        return err(LuceneError::illegal_argument("k must be < to"));
    }
    return {};
}

} // namespace lucene
