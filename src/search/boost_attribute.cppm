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

export module lucene_core:boost_attribute;

import :types;
import std;

import :attributes;

namespace lucene {

/// The per-term boost a terms enum publishes while a multi-term query is
/// rewritten, so that the rewrite can weigh the terms it collects.
///
/// Port of `BoostAttribute` + `BoostAttributeImpl`
/// (`core/search/boost_attribute.rs`, `boost_attribute_impl.rs`).
///
/// A terms enum adds this attribute to itself; the rewrite reads it after each
/// [`TermsEnum::next`](:terms_enum).  `FuzzyQuery` uses it to weigh the edit
/// distance of every term.
///
/// **Deviation:** the reference implementation splits the trait from its
/// implementation; the port has one concrete attribute class.
export class BoostAttribute final : public Attribute {
public:
    BoostAttribute() : Attribute(attribute_type_tag<BoostAttribute>()) {}

    /// The boost of a term that does not set one.
    static constexpr float kDefaultBoost = 1.0F;

    [[nodiscard]] std::unique_ptr<Attribute> clone() const override {
        return std::make_unique<BoostAttribute>(*this);
    }

    void clear() override { boost = kDefaultBoost; }

    [[nodiscard]] std::string_view name() const override { return "BoostAttribute"; }

    /// Sets the boost of the current term.
    void set_boost(float value) noexcept { boost = value; }
    /// The boost of the current term, `1.0` by default.
    [[nodiscard]] float get_boost() const noexcept { return boost; }

    /// The boost of the current term.
    float boost{kDefaultBoost};
};

} // namespace lucene
