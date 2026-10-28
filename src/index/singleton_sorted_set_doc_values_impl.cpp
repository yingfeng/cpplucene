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

module lucene_core:singleton_sorted_set_doc_values.impl;

import :singleton_sorted_set_doc_values;
import :types;
import std;

import :bytes_ref;
import :doc_values_iterator;
import :error;
import :sorted_doc_values;
import :sorted_doc_values_terms_enum;
import :sorted_set_doc_values;

namespace lucene {

Result<int32_t> SingletonSortedSetDocValues::next_doc() {
    auto doc = in_->next_doc();
    if (!doc) {
        return std::unexpected(std::move(doc.error()));
    }
    if (doc.value() != NO_MORE_DOCS) {
        auto ord = in_->ord_value();
        if (!ord) {
            return std::unexpected(std::move(ord.error()));
        }
        ord_ = ord.value();
    }
    return doc.value();
}

Result<std::unique_ptr<TermsEnum>> SingletonSortedSetDocValues::terms_enum() {
    if (in_ == nullptr) {
        return err(LuceneError::illegal_state("the cursor has been taken"));
    }
    return std::unique_ptr<TermsEnum>(new SortedDocValuesTermsEnum(in_.get()));
}

Result<int32_t> SingletonSortedSetDocValues::advance(int32_t target) {
    auto doc = in_->advance(target);
    if (!doc) {
        return std::unexpected(std::move(doc.error()));
    }
    if (doc.value() != NO_MORE_DOCS) {
        auto ord = in_->ord_value();
        if (!ord) {
            return std::unexpected(std::move(ord.error()));
        }
        ord_ = ord.value();
    }
    return doc.value();
}

Result<bool> SingletonSortedSetDocValues::advance_exact(int32_t target) {
    auto exact = in_->advance_exact(target);
    if (!exact) {
        return std::unexpected(std::move(exact.error()));
    }
    if (!exact.value()) {
        return false;
    }
    auto ord = in_->ord_value();
    if (!ord) {
        return std::unexpected(std::move(ord.error()));
    }
    ord_ = ord.value();
    return true;
}

Result<int64_t> SingletonSortedSetDocValues::next_ord() {
    // The reference keeps answering the current document's ordinal, so a second
    // call within the same document repeats it instead of returning
    // `NO_MORE_ORDS`.
    return ord_;
}

Result<int32_t> SingletonSortedSetDocValues::doc_value_count() {
    // Always one: a document either has its single value or reports -1 from
    // `next_ord`, which is what the reference implementation does.
    return 1;
}

Result<BytesRef> SingletonSortedSetDocValues::lookup_ord(int64_t ord) {
    if (ord < 0 || ord > std::numeric_limits<int32_t>::max()) {
        return err(LuceneError::illegal_argument(std::format("ord {} is out of range", ord)));
    }
    return in_->lookup_ord(static_cast<int32_t>(ord));
}

Result<int64_t> SingletonSortedSetDocValues::get_value_count() const {
    auto count = in_->get_value_count();
    if (!count) {
        return std::unexpected(std::move(count.error()));
    }
    return static_cast<int64_t>(count.value());
}

} // namespace lucene
