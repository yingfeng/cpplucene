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

export module lucene_core:postings_enum;

import :types;
import std;

import :error;
import :doc_id_set_iterator;

namespace lucene {

/// Iterates through the postings.  Port of the `PostingsEnum` trait
/// (`core/index/postings_enum.rs`).
///
/// NOTE: `next_doc`/`advance` must be called before any of the per-doc methods.
export class PostingsEnum : public DocIdSetIterator {
public:
    /// Term frequency in the current document, or 1 when the field was indexed
    /// with `IndexOptions::Docs` only.
    [[nodiscard]] virtual Result<int32_t> freq() = 0;

    /// Next position in the current document, or -1 when positions were not
    /// indexed.  Calling this more than `freq()` times is undefined.
    [[nodiscard]] virtual Result<int32_t> next_position() = 0;

    /// Start offset of the current position, or -1 when offsets were not
    /// indexed.
    [[nodiscard]] virtual Result<int32_t> start_offset() const = 0;

    /// End offset of the current position, or -1 when offsets were not
    /// indexed.
    [[nodiscard]] virtual Result<int32_t> end_offset() const = 0;

    /// Payload at the current position, or `nullopt` when no payload was
    /// indexed.  The returned bytes are borrowed and must not be modified:
    /// The reference implementation returns `Option<BytesRefValueEnum<'_>>`, which the port spells
    /// as a span into the reader's payload buffer.
    [[nodiscard]] virtual Result<std::optional<std::span<const uint8_t>>> get_payload() const = 0;
};

/// Feature flags of `PostingsEnum` returned to the reader.
inline constexpr int16_t NONE = 0;
inline constexpr int16_t FREQS = 1 << 3;
inline constexpr int16_t POSITIONS = FREQS | 1 << 4;
inline constexpr int16_t OFFSETS = POSITIONS | 1 << 5;
inline constexpr int16_t PAYLOADS = POSITIONS | 1 << 6;
inline constexpr int16_t ALL = OFFSETS | PAYLOADS;

/// True when `feature` is requested by the `flags` word.
[[nodiscard]] constexpr bool feature_requested(int32_t flags, int16_t feature) noexcept {
    return (flags & static_cast<int32_t>(feature)) == static_cast<int32_t>(feature);
}

/// The `either_postingsenum_named!` variants of the reference implementation
/// (`PostingsEnumEnum2`, ...), expressed as one variadic type.
export template <class... Ts>
class PostingsEnumEnum : public PostingsEnum {
public:
    explicit PostingsEnumEnum(std::variant<Ts...> inner) : inner_(std::move(inner)) {}

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

private:
    std::variant<Ts...> inner_;
};

} // namespace lucene
