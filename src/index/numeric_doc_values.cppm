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

import :doc_values_iterator;
import :error;

namespace lucene {

/// A per-document numeric value.  Port of the `NumericDocValues` trait
/// (`core/index/numeric_doc_values.rs`).
export class NumericDocValues : public DocValuesIterator {
public:
    /// The value for the current document.  Calling it after
    /// [`DocValuesIterator::advance_exact`] returned `false` is illegal.
    [[nodiscard]] virtual Result<int64_t> long_value() = 0;
};

/// The `either_numeric_docvalues!` variants of rlucene
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
