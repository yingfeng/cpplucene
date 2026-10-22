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

export module lucene_core:impacts_enum;

import :types;
import std;

import :error;
import :impacts;
import :impacts_source;
import :postings_enum;

namespace lucene {

/// Extension of [`PostingsEnum`] that also provides information about
/// upcoming impacts.  Port of the `ImpactsEnum` trait
/// (`core/index/impacts_enum.rs`, `PostingsEnum + ImpactsSource`).
export class ImpactsEnum : public PostingsEnum, public ImpactsSource {};

/// The `either_impactsenum_named!` variants of rlucene (`ImpactsEnumEnum2`,
/// ...), expressed as one variadic type.
export template <class... Ts>
class ImpactsEnumEnum : public ImpactsEnum {
public:
    explicit ImpactsEnumEnum(std::variant<Ts...> inner) : inner_(std::move(inner)) {}

    // ---- DocIdSetIterator ----

    [[nodiscard]] int32_t doc_id() const override {
        return std::visit([](const auto &enumeration) { return enumeration.doc_id(); }, inner_);
    }
    [[nodiscard]] Result<int32_t> next_doc() override {
        return std::visit([](auto &enumeration) { return enumeration.next_doc(); }, inner_);
    }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        return std::visit([target](auto &enumeration) { return enumeration.advance(target); }, inner_);
    }
    [[nodiscard]] Result<int32_t> slow_advance(int32_t target) override {
        return std::visit([target](auto &enumeration) { return enumeration.slow_advance(target); }, inner_);
    }
    [[nodiscard]] Result<int64_t> cost() const override {
        return std::visit([](const auto &enumeration) { return enumeration.cost(); }, inner_);
    }

    // ---- PostingsEnum ----

    [[nodiscard]] Result<int32_t> freq() override {
        return std::visit([](auto &enumeration) { return enumeration.freq(); }, inner_);
    }
    [[nodiscard]] Result<int32_t> next_position() override {
        return std::visit([](auto &enumeration) { return enumeration.next_position(); }, inner_);
    }
    [[nodiscard]] Result<int32_t> start_offset() const override {
        return std::visit([](const auto &enumeration) { return enumeration.start_offset(); }, inner_);
    }
    [[nodiscard]] Result<int32_t> end_offset() const override {
        return std::visit([](const auto &enumeration) { return enumeration.end_offset(); }, inner_);
    }
    [[nodiscard]] Result<std::optional<std::span<const uint8_t>>> get_payload() const override {
        return std::visit([](const auto &enumeration) { return enumeration.get_payload(); }, inner_);
    }

    // ---- ImpactsSource ----

    [[nodiscard]] VoidResult advance_shallow(int32_t target) override {
        return std::visit([target](auto &enumeration) { return enumeration.advance_shallow(target); }, inner_);
    }
    [[nodiscard]] Result<std::unique_ptr<Impacts>> get_impacts() const override {
        return std::visit([](const auto &enumeration) { return enumeration.get_impacts(); }, inner_);
    }

private:
    std::variant<Ts...> inner_;
};

} // namespace lucene
