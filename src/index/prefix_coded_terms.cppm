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

export module lucene_core:prefix_coded_terms;

import :types;
import std;

import :byte_array_output;
import :bytes_ref;
import :data_input;
import :error;

namespace lucene {

export class PrefixCodedTerms;

/// An iterator over the terms of a [`PrefixCodedTerms`].
///
/// Port of `TermIterator` (`core/index/prefix_coded_terms.rs`).
///
/// **Deviation:** the reference implementation keeps the iterator's buffer with
/// a `Cow`, because its decoded terms borrow the content for some builds and own
/// it for others.  The port always decodes into an owned [`BytesRef`], so the
/// buffer keeps the prefix of the previous term alive between two calls.
export class TermIterator {
public:
    TermIterator() = default;

    /// The next term, or `nullptr` at the end of the enumeration.
    [[nodiscard]] Result<const BytesRef *> next();

    /// The field of the term last returned by [`next`]; empty before the first
    /// call.
    [[nodiscard]] const std::string &field() const noexcept { return field_; }

    /// The generation of the segment this term data belongs to.
    [[nodiscard]] int64_t del_gen() const noexcept { return del_gen_; }

private:
    friend class PrefixCodedTerms;

    TermIterator(std::shared_ptr<const std::vector<uint8_t>> content, int64_t del_gen);

    std::shared_ptr<const std::vector<uint8_t>> content_{};
    ByteArrayDataInput input_{};
    BytesRef term_{};
    std::string field_{};
    int64_t del_gen_{0};
};

/// A sorted set of terms, stored compressed with a shared prefix between
/// consecutive terms.
///
/// Port of `PrefixCodedTerms` (`core/index/prefix_coded_terms.rs`).
///
/// **Deviations:**
///  * the reference implementation keeps the encoded content in
///    `Arc<[u8]>`/borrowed slices; the port always owns it through a
///    `shared_ptr<const std::vector<uint8_t>>`, so copies are cheap;
///  * `PrefixCodedTermsReader` (the index-level registry used by serialized
///    queries) is not ported: nothing in this port serializes a term dictionary
///    across processes yet.
export class PrefixCodedTerms {
public:
    /// Collects the terms of a query; terms have to be added in increasing
    /// field-then-term order.
    class Builder {
    public:
        /// Appends `term` of `field`.
        [[nodiscard]] VoidResult add(std::string_view field, const BytesRef &term);

        /// The collected terms; empty when nothing was added.
        [[nodiscard]] PrefixCodedTerms build() const;

    private:
        /// Encoded `(prefix << 1 | new_field, [field], suffix_length, suffix)`
        /// records.
        ByteArrayOutput output_{"prefix-coded-terms"};
        std::optional<BytesRef> last_term_{};
        std::string last_field_{};
        int64_t size_{0};
    };

    PrefixCodedTerms() = default;

    /// Number of terms, not counting duplicates across field changes.
    [[nodiscard]] int64_t size() const noexcept { return size_; }

    /// Whether there are no terms.
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    /// Sets the generation of the segment the terms were read from; this is
    /// metadata only and does not participate in equality.
    void set_del_gen(int64_t del_gen) noexcept { del_gen_ = del_gen; }

    /// The generation of the segment the terms were read from.
    [[nodiscard]] int64_t del_gen() const noexcept { return del_gen_; }

    /// An iterator over the terms, in increasing order.
    [[nodiscard]] TermIterator iterator() const;

    [[nodiscard]] bool operator==(const PrefixCodedTerms &other) const;

    [[nodiscard]] size_t hash() const;

private:
    friend class TermIterator;

    std::shared_ptr<const std::vector<uint8_t>> content_{};
    int64_t size_{0};
    int64_t del_gen_{0};
};

} // namespace lucene
