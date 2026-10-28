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

export module lucene_core:sorted_doc_values_terms_enum;

import :types;
import std;

import :attribute_source;
import :bytes_ref;
import :impacts_enum;
import :postings_enum;
import :sorted_doc_values;
import :term_state;
import :terms_enum;

namespace lucene {

/// Enumerates the terms of a [`SortedDocValues`], i.e. its dictionary in
/// ordinal order.
///
/// Port of `SortedDocValuesTermsEnum` (`core/index/sorted_doc_values_terms_enum.rs`).
///
/// **Lifetime:** the cursor is borrowed, so the enum must not outlive it; the
/// reference implementation shares ownership instead (`Rc<RefCell<S>>`).
///
/// **Deviations:** the postings are a
/// [`DummyPostingsEnum`](lucene_core:dummy_postings_enum) (doc values carry no
/// postings), impacts are unsupported, and `attributes()` reports
/// `unsupported_operation` because this port has no empty attribute source.
export class SortedDocValuesTermsEnum final : public TermsEnum {
public:
    /// An enum over `values`, which must outlive it.
    explicit SortedDocValuesTermsEnum(SortedDocValues *values) : values_(values) {}

    [[nodiscard]] Result<const BytesRef *> next() override;

    [[nodiscard]] Result<const BytesRef *> term() const override;

    [[nodiscard]] Result<int64_t> ord() const override;

    [[nodiscard]] Result<int32_t> doc_freq() override;
    [[nodiscard]] Result<int64_t> total_term_freq() override;

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

    /// The ordinal this enum is positioned on, or `-1` before the first
    /// [`next`].
    [[nodiscard]] int32_t current_ord() const noexcept { return current_ord_; }

private:
    /// Loads the term of `ord` into the scratch buffer and returns it.
    [[nodiscard]] Result<const BytesRef *> load_ord(int32_t ord) const;

    SortedDocValues *values_;
    /// The reference keeps a plain `-1` sentinel and does not clear it when a
    /// seek fails, so a following [`next`] resumes from the old position.
    int32_t current_ord_{-1};
    mutable BytesRef buffer_{};
};

} // namespace lucene
