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

export module lucene_core:numeric_doc_values;

import :types;
import std;

import :bit_util;
import :doc_values_iterator;
import :error;

namespace lucene {

/// A direct, ordinal addressed view of a numeric field where every document has
/// a value, i.e. the layout of the norms and doc-values encodings.
///
/// **Deviation:** the reference implementation gets this for free by
/// monomorphising its scorers over the concrete doc-values types.  This port
/// keeps the abstract interface, so the scorers ask for the raw view once
/// instead: one bounds check and one load per document instead of two virtual
/// calls plus a `Result`.
export struct DirectNumericValues {
    /// The raw values, `width` bytes each; empty for the singleton form.
    std::span<const uint8_t> bytes;
    /// The value shared by every document, used when `width` is zero.
    int64_t singleton{0};
    /// `1`, `2`, `4` or `8`, or zero for the singleton form.
    int8_t width{0};
    /// One past the last document with a value.
    int32_t max_doc{0};

    /// The value of `ordinal`, or `false` when it has none.
    [[nodiscard]] bool value_at(int32_t ordinal, int64_t &out) const noexcept {
        if (ordinal < 0 || ordinal >= max_doc) {
            return false;
        }
        if (width == 0) {
            out = singleton;
            return true;
        }
        const size_t offset = static_cast<size_t>(ordinal) * static_cast<size_t>(width);
        switch (width) {
            case 1:
                out = static_cast<int64_t>(static_cast<int8_t>(bytes[offset]));
                return true;
            case 2:
                out = static_cast<int64_t>(BitUtil::get_i16_le(bytes, offset));
                return true;
            case 4:
                out = static_cast<int64_t>(BitUtil::get_i32_le(bytes, offset));
                return true;
            default:
                out = BitUtil::get_i64_le(bytes, offset);
                return true;
        }
    }
};

/// A per-document numeric value.  Port of the `NumericDocValues` trait
/// (`core/index/numeric_doc_values.rs`).
export class NumericDocValues : public DocValuesIterator {
public:
    /// The value for the current document.  Calling it after
    /// [`DocValuesIterator::advance_exact`] returned `false` is illegal.
    [[nodiscard]] virtual Result<int64_t> long_value() = 0;

    /// [`DirectNumericValues`] over the same values, when the encoding allows it
    /// (dense or singleton values); `nullopt` otherwise.
    [[nodiscard]] virtual std::optional<DirectNumericValues> direct_values() const { return std::nullopt; }

    /// [`advance_exact`] followed by [`long_value`], in one call: `out` is only
    /// written when the document has a value.  The scorers use it when the
    /// encoding has no direct view, so a document costs one virtual call instead
    /// of two.
    [[nodiscard]] virtual Result<bool> exact_value(int32_t doc, int64_t &out) {
        auto exact = advance_exact(doc);
        if (!exact.has_value()) {
            return std::unexpected(std::move(exact.error()));
        }
        if (!exact.value()) {
            return false;
        }
        auto value = long_value();
        if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
        }
        out = value.value();
        return true;
    }
};

/// The `either_numeric_docvalues!` variants of the reference implementation
/// (`NumericDocValuesEnum2`, ...), expressed as one variadic type.
export template <class... Ts>
class NumericDocValuesEnum : public NumericDocValues {
public:
    explicit NumericDocValuesEnum(std::variant<Ts...> inner) : inner_(std::move(inner)) {}

    [[nodiscard]] int32_t doc_id() const override {
        return std::visit([](const auto &values) { return values.doc_id(); }, inner_);
    }
    [[nodiscard]] Result<int32_t> next_doc() override {
        return std::visit([](auto &values) { return values.next_doc(); }, inner_);
    }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        return std::visit([target](auto &values) { return values.advance(target); }, inner_);
    }
    [[nodiscard]] Result<int32_t> slow_advance(int32_t target) override {
        return std::visit([target](auto &values) { return values.slow_advance(target); }, inner_);
    }
    [[nodiscard]] Result<int64_t> cost() const override {
        return std::visit([](const auto &values) { return values.cost(); }, inner_);
    }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        return std::visit([target](auto &values) { return values.advance_exact(target); }, inner_);
    }
    [[nodiscard]] Result<int64_t> long_value() override {
        return std::visit([](auto &values) { return values.long_value(); }, inner_);
    }

private:
    std::variant<Ts...> inner_;
};

} // namespace lucene
