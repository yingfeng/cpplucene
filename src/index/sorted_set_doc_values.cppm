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

export module lucene_core:sorted_set_doc_values;

import :types;
import std;

import :bytes_ref;
import :doc_values_iterator;
import :error;
import :terms_enum;

namespace lucene {

/// Sentinel returned by [`SortedSetDocValues::next_ord`] once the current
/// document has no more values.
export inline constexpr int64_t NO_MORE_ORDS = -1;

/// A multi-valued version of [`SortedDocValues`]: the values of a document are
/// deduplicated, dereferenced and sorted into one dictionary, and each document
/// reports a run of increasing ordinals.
///
/// Port of the `SortedSetDocValues` trait (`core/index/sorted_set_doc_values.rs`).
/// `terms_enum()` has the same deviation as its single-valued counterpart: the
/// adapter arrives with the search layer, and [`lookup_term`] covers lookups.
export class SortedSetDocValues : public DocValuesIterator {
public:
    /// The next ordinal of the current document, or [`NO_MORE_ORDS`].
    [[nodiscard]] virtual Result<int64_t> next_ord() = 0;

    /// How many ordinals the current document has; always at least one.
    [[nodiscard]] virtual Result<int32_t> doc_value_count() = 0;

    /// The term of `ord`.
    [[nodiscard]] virtual Result<BytesRef> lookup_ord(int64_t ord) = 0;

    /// One plus the highest ordinal, i.e. the dictionary size.
    [[nodiscard]] virtual Result<int64_t> get_value_count() const = 0;

    /// The ordinal of `key`, or `-insertion_point - 1` when it is not present.
    ///
    /// The default implementation mirrors rlucene: a binary search over the
    /// dictionary through [`lookup_ord`].
    [[nodiscard]] virtual Result<int64_t> lookup_term(std::span<const uint8_t> key);

    /// A [`TermsEnum`] over the dictionary.
    [[nodiscard]] virtual Result<std::unique_ptr<TermsEnum>> terms_enum() {
        return err(LuceneError::not_implemented("terms_enum is not supported by this SortedSetDocValues"));
    }
};

} // namespace lucene
