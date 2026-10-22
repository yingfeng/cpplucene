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

module lucene_core:limited_finite_strings_iterator.impl;

import :limited_finite_strings_iterator;

import :automaton;
import :error;
import :finite_strings_iterator;
import :ints_ref;
import :types;

import std;

namespace lucene {

LimitedFiniteStringsIterator::LimitedFiniteStringsIterator(const FiniteStringsIterator &base, int32_t limit)
    : limit_(limit), base_(base) {}

Result<LimitedFiniteStringsIterator> LimitedFiniteStringsIterator::create(const Automaton &a, int32_t limit) {
    if (limit != -1 && limit <= 0) {
        return err(LuceneError::illegal_argument(
                std::format("limit must be -1 (which means no limit), or > 0; got: {}", limit)));
    }

    const int32_t effective_limit = limit > 0 ? limit : std::numeric_limits<int32_t>::max();
    return LimitedFiniteStringsIterator(FiniteStringsIterator(a), effective_limit);
}

Result<std::optional<IntsRef>> LimitedFiniteStringsIterator::next() {
    if (count_ >= limit_) {
        return std::optional<IntsRef>{};
    }

    auto result = base_.next();
    if (!result) {
        return err(std::move(result.error()));
    }
    if (!result->has_value()) {
        return std::optional<IntsRef>{};
    }
    count_ += 1;
    return result;
}

} // namespace lucene
