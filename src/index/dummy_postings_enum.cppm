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

export module lucene_core:dummy_postings_enum;

import :types;
import std;

import :doc_id_set_iterator;
import :error;
import :postings_enum;

namespace lucene {

/// A [`PostingsEnum`] over no documents at all.
///
/// Port of `DummyPostingsEnum` (`core/index/dummy/dummy_postings_enum.rs`): term
/// enumerations that carry no postings (the doc values ones, for instance) still
/// have to answer [`TermsEnum::postings_with_flags`](lucene_core:terms_enum), and
/// answering "no documents" is the honest reply.
export class DummyPostingsEnum final : public PostingsEnum {
public:
    [[nodiscard]] int32_t doc_id() const override { return NO_MORE_DOCS; }
    [[nodiscard]] Result<int32_t> next_doc() override { return NO_MORE_DOCS; }
    [[nodiscard]] Result<int32_t> advance(int32_t /*target*/) override { return NO_MORE_DOCS; }
    [[nodiscard]] Result<int64_t> cost() const override { return 0; }

    [[nodiscard]] Result<int32_t> freq() override {
        return err(LuceneError::unsupported_operation("a dummy postings enum has no frequencies"));
    }
    [[nodiscard]] Result<int32_t> next_position() override {
        return err(LuceneError::unsupported_operation("a dummy postings enum has no positions"));
    }
    [[nodiscard]] Result<int32_t> start_offset() const override {
        return err(LuceneError::unsupported_operation("a dummy postings enum has no offsets"));
    }
    [[nodiscard]] Result<int32_t> end_offset() const override {
        return err(LuceneError::unsupported_operation("a dummy postings enum has no offsets"));
    }
    [[nodiscard]] Result<std::optional<std::span<const uint8_t>>> get_payload() const override {
        return std::optional<std::span<const uint8_t>>{};
    }
};

} // namespace lucene
