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

export module lucene_core:sorted_set_doc_values_terms_enum;

import :types;
import std;

import :attribute_source;
import :bytes_ref;
import :error;
import :impacts_enum;
import :postings_enum;
import :sorted_set_doc_values;
import :term_state;
import :terms_enum;

namespace lucene {

/// Enumerates the terms of a [`SortedSetDocValues`]: its dictionary, in ordinal
/// order.
///
/// Port of `SortedSetDocValuesTermsEnum`
/// (`core/index/sorted_set_doc_values_terms_enum.rs`).
///
/// The doc values are shared, not copied: like the reference implementation
/// (`Rc<RefCell<S>>`), the enum and the [`Terms`](lucene_core:terms) it came from
/// read through the same cursor, so only one of them may be used at a time.
///
/// **Deviations:**
///  * the postings are a [`DummyPostingsEnum`](lucene_core:dummy_postings_enum)
///    and the impacts are unsupported, because doc values carry neither;
///  * `doc_freq`/`total_term_freq` report `1`, the reference implementation's
///    answer for a term it cannot count.
export class SortedSetDocValuesTermsEnum final : public TermsEnum {
public:
    /// An enum over `values`.
    explicit SortedSetDocValuesTermsEnum(std::shared_ptr<SortedSetDocValues> values);

    [[nodiscard]] Result<const BytesRef *> next() override;

    [[nodiscard]] Result<const BytesRef *> term() const override;

    [[nodiscard]] Result<int64_t> ord() const override;

    [[nodiscard]] Result<int32_t> doc_freq() override { return 1; }
    [[nodiscard]] Result<int64_t> total_term_freq() override { return 1; }

    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>> postings_with_flags(std::unique_ptr<PostingsEnum> reuse,
                                                                           int32_t flags) override;

    [[nodiscard]] Result<std::unique_ptr<ImpactsEnum>> impacts(int32_t flags) override;

    [[nodiscard]] Result<AttributeSource *> attributes() override;
    [[nodiscard]] Result<AttributeSource *> attributes_mut() override;

    [[nodiscard]] Result<bool> seek_exact(std::span<const uint8_t> target) override;
    [[nodiscard]] Result<bool> prepare_seek_exact(std::span<const uint8_t> target) override;
    [[nodiscard]] Result<bool> get_prepare_seek_exact_status(std::span<const uint8_t> target) override;
    [[nodiscard]] Result<SeekStatus> seek_ceil(std::span<const uint8_t> target) override;
    [[nodiscard]] VoidResult seek_exact_with_ord(int64_t ord) override;
    [[nodiscard]] VoidResult seek_exact_with_state(std::span<const uint8_t> term, const TermStateEnum &state) override;
    [[nodiscard]] Result<TermStateEnum> term_state() override;

private:
    /// The ordinal of the term this enum is positioned on.
    [[nodiscard]] Result<int64_t> required_ord() const;

    std::shared_ptr<SortedSetDocValues> values_;
    std::optional<int64_t> ord_{};
    mutable BytesRef buffer_{};
};

} // namespace lucene
