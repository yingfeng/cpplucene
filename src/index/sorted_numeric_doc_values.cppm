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

export module lucene_core:sorted_numeric_doc_values;

import :types;
import std;

import :doc_values_iterator;
import :error;
import :numeric_doc_values;

namespace lucene {

/// A pre-sorted `i64[]` per document.
///
/// Port of the `SortedNumericDocValues` trait
/// (`core/index/sorted_numeric_doc_values.rs`): the values of one document are
/// visited with [`next_value`], at most [`doc_value_count`] times.
export class SortedNumericDocValues : public DocValuesIterator {
public:
    /// The next value of the current document.
    [[nodiscard]] virtual Result<int64_t> next_value() = 0;

    /// How many values the current document has; always at least one.
    [[nodiscard]] virtual Result<int32_t> doc_value_count() = 0;

    /// Whether every document has exactly one value, which lets callers use the
    /// single-valued API.
    [[nodiscard]] virtual bool is_single_valued() const { return false; }

    /// A view of the current document's only value, when there is exactly one.
    [[nodiscard]] virtual Result<std::unique_ptr<NumericDocValues>> get_numeric_doc_values() {
        return err(LuceneError::unsupported_operation("this SortedNumericDocValues is not single-valued"));
    }
};

} // namespace lucene
