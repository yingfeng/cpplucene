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

export module lucene_core:doc_values_iterator;

import :types;
import std;

import :doc_id_set_iterator;
import :error;

namespace lucene {

/// Iterates over doc values.  Port of the `DocValuesIterator` trait
/// (`core/index/doc_values_iterator.rs`).
export class DocValuesIterator : public DocIdSetIterator {
public:
    /// Advances to exactly `target` and returns whether it has a value.
    ///
    /// `target` must be greater than or equal to the current [`doc_id`] and a
    /// valid doc id.  After the call [`doc_id`] returns `target`.
    ///
    /// The default implementation mirrors the reference implementation and reports
    /// `not_implemented`.
    [[nodiscard]] virtual Result<bool> advance_exact(int32_t target) {
        return err(LuceneError::not_implemented(std::format("advance_exact({}) is not supported", target)));
    }
};

} // namespace lucene
