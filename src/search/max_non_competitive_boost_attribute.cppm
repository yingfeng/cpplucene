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

export module lucene_core:max_non_competitive_boost_attribute;

import :types;
import std;

import :attributes;
import :bytes_ref;

namespace lucene {

/// The boost below which a term is no longer worth collecting, published by a
/// top-terms rewrite so the terms enum can stop early.
///
/// Port of `MaxNonCompetitiveBoostAttribute` +
/// `MaxNonCompetitiveBoostAttributeImpl`
/// (`core/search/max_non_competitive_boost_attribute.rs`,
/// `max_non_competitive_boost_attribute_impl.rs`).
///
/// The rewrite adds the attribute to one source shared by all segments;
/// `FuzzyQuery` reads it to stop enumerating terms that cannot make it into the
/// top list any more.
///
/// **Deviation:** the reference implementation splits the trait from its
/// implementation; the port has one concrete attribute class.
export class MaxNonCompetitiveBoostAttribute final : public Attribute {
public:
    MaxNonCompetitiveBoostAttribute() : Attribute(attribute_type_tag<MaxNonCompetitiveBoostAttribute>()) {}

    [[nodiscard]] std::unique_ptr<Attribute> clone() const override {
        return std::make_unique<MaxNonCompetitiveBoostAttribute>(*this);
    }

    void clear() override {
        max_non_competitive_boost = -std::numeric_limits<float>::infinity();
        competitive_term.reset();
    }

    [[nodiscard]] std::string_view name() const override { return "MaxNonCompetitiveBoostAttribute"; }

    /// Sets the maximum boost that is no longer competitive.
    void set_max_non_competitive_boost(float value) noexcept { max_non_competitive_boost = value; }
    /// The maximum boost that is no longer competitive; negative infinity means
    /// every term is still competitive.
    [[nodiscard]] float get_max_non_competitive_boost() const noexcept { return max_non_competitive_boost; }

    /// The term that made the boost change, if any.
    void set_competitive_term(std::optional<BytesRef> term) { competitive_term = std::move(term); }
    /// The term that made the boost change, if any.
    [[nodiscard]] const std::optional<BytesRef> &get_competitive_term() const noexcept { return competitive_term; }

    /// The maximum boost that is no longer competitive.
    float max_non_competitive_boost{-std::numeric_limits<float>::infinity()};
    /// The term that made the boost change.
    std::optional<BytesRef> competitive_term{};
};

} // namespace lucene
