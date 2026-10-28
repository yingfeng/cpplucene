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

export module lucene_core:selector;

import :types;
import std;

import :error;

namespace lucene {

/// A selection algorithm: computes the `k`-th greatest value of a collection.
///
/// Port of the `Selector` trait (`util/selector.rs`).  Like [`Sorter`] it only
/// sees *slots* of an array the caller owns.
export class Selector {
public:
    virtual ~Selector() = default;

    /// Reorders `[from, to)` so that slot `k` holds the value it would hold if
    /// the range were sorted; `[from, k)` only contains values less than or
    /// equal to it and `(k, to)` only values greater than or equal to it.
    [[nodiscard]] virtual VoidResult select(size_t /*from*/, size_t /*to*/, size_t /*k*/);

    /// Swaps the values in slots `i` and `j`.
    [[nodiscard]] virtual VoidResult swap(size_t /*i*/, size_t /*j*/);

    /// Rejects `k < from` and `k >= to`.
    [[nodiscard]] VoidResult check_args(size_t from, size_t to, size_t k) const;
};

} // namespace lucene
