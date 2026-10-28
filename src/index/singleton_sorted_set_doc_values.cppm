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

export module lucene_core:singleton_sorted_set_doc_values;

import :types;
import std;

import :bytes_ref;
import :doc_values_iterator;
import :error;
import :sorted_doc_values;
import :sorted_doc_values_terms_enum;
import :sorted_set_doc_values;

namespace lucene {

/// Presents a [`SortedDocValues`] as a single-valued [`SortedSetDocValues`].
///
/// Port of `SingletonSortedSetDocValues`
/// (`index/singleton_sorted_set_doc_values.rs`): each document has at most one
/// ordinal, so [`next_ord`] returns it once and then [`NO_MORE_ORDS`], and
/// [`doc_value_count`] is `1` for documents that have a value.
export class SingletonSortedSetDocValues final : public SortedSetDocValues {
public:
    explicit SingletonSortedSetDocValues(std::unique_ptr<SortedDocValues> in) : in_(std::move(in)) {}

    /// The wrapped single-valued cursor, or null.
    [[nodiscard]] SortedDocValues *in() const noexcept { return in_.get(); }

    // ---- DocIdSetIterator ----

    [[nodiscard]] int32_t doc_id() const override { return in_->doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override { return in_->cost(); }

    // ---- DocValuesIterator ----

    [[nodiscard]] Result<bool> advance_exact(int32_t target) override;

    // ---- SortedSetDocValues ----

    [[nodiscard]] Result<int64_t> next_ord() override;
    [[nodiscard]] Result<int32_t> doc_value_count() override;
    [[nodiscard]] Result<BytesRef> lookup_ord(int64_t ord) override;
    [[nodiscard]] Result<int64_t> get_value_count() const override;
    /// The dictionary, enumerated through the wrapped single-valued cursor.
    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> terms_enum() override;

    /// A [`SingletonSortedSetDocValues`] always has at most one value.
    [[nodiscard]] bool is_single_valued() const noexcept { return true; }

    /// Hands the wrapped single-valued cursor over and leaves this wrapper empty.
    ///
    /// The doc values rewrite uses it once it knows the field has at most one
    /// value per document: probing the single cursor is cheaper than walking a
    /// sorted set of one element, which is what the reference implementation's
    /// `unwrap_singleton_sorted` does.
    [[nodiscard]] std::unique_ptr<SortedDocValues> release() noexcept { return std::move(in_); }

private:
    /// Ordinal already handed out for the current document, or -1.
    int64_t ord_{-1};

    std::unique_ptr<SortedDocValues> in_;
};

} // namespace lucene
