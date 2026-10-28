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

export module lucene_core:double_values;

import :types;
import std;

import :error;

namespace lucene {

/// Per-document `double` values, as consumed by the sorting and scoring
/// machinery.
///
/// Port of the `DoubleValues` trait (`search/double_values.rs`).
export class DoubleValues {
public:
    virtual ~DoubleValues() = default;

    /// The value of the current document; call [`advance_exact`] first.
    [[nodiscard]] virtual Result<double> double_value() const = 0;

    /// Positions on `doc`, returning whether it has a value.
    [[nodiscard]] virtual Result<bool> advance_exact(int32_t doc) = 0;
};

/// [`DoubleValues`] that falls back to a default when the wrapped values have
/// none for the current document.
///
/// Port of `WithDefaultDoubleValues` (`search/double_values.rs`).
export class WithDefaultDoubleValues final : public DoubleValues {
public:
    WithDefaultDoubleValues(std::unique_ptr<DoubleValues> values, double missing_value)
        : values_(std::move(values)), missing_value_(missing_value) {}

    [[nodiscard]] Result<double> double_value() const override;
    [[nodiscard]] Result<bool> advance_exact(int32_t doc) override;

private:
    std::unique_ptr<DoubleValues> values_;
    double missing_value_;
    bool has_value_{false};
};

/// [`DoubleValues`] with no values at all.
///
/// Port of `EmptyDoubleValues` (`search/double_values.rs`): reading a value is
/// an error, advancing never finds one.
export class EmptyDoubleValues final : public DoubleValues {
public:
    [[nodiscard]] Result<double> double_value() const override {
        return err(LuceneError::unsupported_operation("EmptyDoubleValues has no values"));
    }
    [[nodiscard]] Result<bool> advance_exact(int32_t /*doc*/) override { return false; }
};

} // namespace lucene
