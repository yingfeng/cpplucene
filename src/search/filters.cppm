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

export module lucene_core:search_filters;

import :types;
import std;

import :doc_id_set_iterator;
import :doc_values_iterator;
import :error;
import :numeric_doc_values;
import :scorable;

namespace lucene {

/// A [`DocIdSetIterator`] that delegates to another one.
///
/// Port of `FilterDocIdSetIterator` (`search/filter_doc_id_set_iterator.rs`).
export class FilterDocIdSetIterator : public DocIdSetIterator {
public:
    explicit FilterDocIdSetIterator(std::unique_ptr<DocIdSetIterator> in) : in_(std::move(in)) {}

    [[nodiscard]] int32_t doc_id() const override { return in_->doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override { return in_->next_doc(); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override { return in_->advance(target); }
    [[nodiscard]] Result<int32_t> slow_advance(int32_t target) override { return in_->slow_advance(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return in_->cost(); }

    [[nodiscard]] DocIdSetIterator &in() const noexcept { return *in_; }

protected:
    std::unique_ptr<DocIdSetIterator> in_;
};

/// A [`Scorable`] that delegates to another one.
///
/// Port of `FilterScorable` (`search/filter_scorable.rs`).
export class FilterScorable : public Scorable {
public:
    explicit FilterScorable(Scorable &in) : in_(&in) {}

    [[nodiscard]] Result<float> score() override { return in_->score(); }
    [[nodiscard]] VoidResult set_min_competitive_score(float min_score) override {
        return in_->set_min_competitive_score(min_score);
    }
    [[nodiscard]] Scorable &in() const noexcept { return *in_; }

protected:
    Scorable *in_;
};

/// A [`NumericDocValues`] cursor that delegates to another one.
///
/// Port of `FilterNumericDocValues` (`index/filter_numeric_doc_values.rs`).
export class FilterNumericDocValues : public NumericDocValues {
public:
    explicit FilterNumericDocValues(std::unique_ptr<NumericDocValues> in) : in_(std::move(in)) {}

    [[nodiscard]] int32_t doc_id() const override { return in_->doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override { return in_->next_doc(); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override { return in_->advance(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return in_->cost(); }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override { return in_->advance_exact(target); }
    [[nodiscard]] Result<int64_t> long_value() override { return in_->long_value(); }

    [[nodiscard]] NumericDocValues &in() const noexcept { return *in_; }

protected:
    std::unique_ptr<NumericDocValues> in_;
};

} // namespace lucene
