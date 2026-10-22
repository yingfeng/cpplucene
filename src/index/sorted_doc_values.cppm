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

export module lucene_core:sorted_doc_values;

import :types;
import std;

import :bytes_ref;
import :doc_values_iterator;
import :error;
import :terms_enum;

namespace lucene {

/// A single-valued per-document term, deduplicated into a dictionary of sorted
/// values.
///
/// Port of the `SortedDocValues` trait (`core/index/sorted_doc_values.rs`): each
/// document points at one ordinal, and ordinals are dense and in sorted order.
///
/// **Deviation:** `terms_enum()` reports `not_implemented` by default.  The
/// [`SortedDocValuesTermsEnum`] adapter needs the dummy postings/impacts enums of
/// the search layer, which this port does not have yet; every codec that writes
/// sorted doc values works without it, and `lookup_term` covers the lookups the
/// merge path needs.
export class SortedDocValues : public DocValuesIterator {
public:
    /// The ordinal of the current document, or `-1` when it has no value.
    [[nodiscard]] virtual Result<int32_t> ord_value() = 0;

    /// The term of `ord`.
    [[nodiscard]] virtual Result<BytesRef> lookup_ord(int32_t ord) = 0;

    /// One plus the highest ordinal, i.e. the dictionary size.
    [[nodiscard]] virtual Result<int32_t> get_value_count() const = 0;

    /// The ordinal of `key`, or `-insertion_point - 1` when it is not present.
    ///
    /// The default implementation mirrors rlucene: a binary search over the
    /// dictionary through [`lookup_ord`].
    [[nodiscard]] virtual Result<int32_t> lookup_term(std::span<const uint8_t> key);

    /// A [`TermsEnum`] over the dictionary.
    [[nodiscard]] virtual Result<std::unique_ptr<TermsEnum>> terms_enum() {
        return err(LuceneError::not_implemented("terms_enum is not supported by this SortedDocValues"));
    }
};

} // namespace lucene
