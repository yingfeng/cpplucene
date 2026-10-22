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

export module lucene_core:doc_id_set;

import :types;
import std;

import :bits;
import :doc_id_set_iterator;
import :error;

namespace lucene {

/// A set of document ids, opened as an iterator (and optionally as random
/// access bits).
///
/// Port of the `DocIdSet` trait (`core/search/doc_id_set.rs`).
///
/// **Deviation:** rlucene's `bits()` returns the concrete bit set through a
/// generic associated type; here it is an opaque `Bits`, which is all the
/// consumers in the ported code need.  `finish()` stays, because some sets
/// (e.g. `DocsWithFieldSet`) only become iterable after it.
export class DocIdSet {
public:
    virtual ~DocIdSet() = default;

    /// An iterator over the matching documents.
    [[nodiscard]] virtual Result<std::unique_ptr<DocIdSetIterator>> iterator() const = 0;

    /// A random-access view, or `nullptr` when the set cannot provide one.
    /// `nullptr` does **not** mean that no document matches.
    [[nodiscard]] virtual Result<std::shared_ptr<const Bits>> bits() const {
        return std::shared_ptr<const Bits>{};
    }

    /// Called before [`iterator`] by sets that assemble themselves lazily.
    [[nodiscard]] virtual VoidResult finish() { return {}; }
};

} // namespace lucene
