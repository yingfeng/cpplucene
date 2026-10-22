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

module lucene_core:single_terms_enum.impl;

import :single_terms_enum;

import :bytes_ref;
import :error;
import :filtered_terms_enum;
import :terms_enum;
import :types;

import std;

namespace lucene {

SingleTermsEnum::SingleTermsEnum(BytesRef single) : single_(std::move(single)) {}

std::unique_ptr<FilteredTermsEnum> SingleTermsEnum::create(std::unique_ptr<TermsEnum> tenum, BytesRef term_text) {
    auto hook = std::make_unique<SingleTermsEnum>(std::move(term_text));
    return FilteredTermsEnum::create(std::move(tenum), std::move(hook), true);
}

Result<std::optional<BytesRef>> SingleTermsEnum::next_seek_term(const BytesRef *current) {
    if (current == nullptr) {
        return std::optional<BytesRef>(single_);
    }
    return std::optional<BytesRef>{};
}

Result<AcceptStatus> SingleTermsEnum::accept(const BytesRef &term, int64_t /*ord*/) {
    if (term == single_) {
        return AcceptStatus::kYes;
    }
    return AcceptStatus::kEnd;
}

} // namespace lucene
