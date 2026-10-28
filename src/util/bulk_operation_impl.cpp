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

module;

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

module lucene_core:bulk_operation.impl;

import :bulk_operation;
import :types;

import std;

namespace lucene {

namespace {

/// Widest integer a window of `bits` bits needs.
///
/// A value of at most 56 bits spans eight bytes whatever its bit offset, so a
/// 64-bit temporary is enough; wider values need a ninth byte and with it a
/// 128-bit window.
template <size_t Bits>
using Window = std::conditional_t<(Bits <= 56), std::uint64_t, unsigned __int128>;

template <size_t Bits>
class PackedBulkOperation final : public BulkOperation {
public:
    void decode(std::span<const uint8_t> blocks, size_t offset, std::span<int64_t> values) const override {
        const size_t available = blocks.size();
        for (size_t i = 0; i < values.size(); ++i) {
            const size_t bit = (offset + i) * Bits;
            const size_t first_byte = bit >> 3;
            const unsigned shift = static_cast<unsigned>(bit & 7U);
            // Bytes the window spans; `(shift + Bits)` bits starting at an
            // arbitrary offset never need more than one extra byte.
            const size_t window_bytes = (shift + Bits + 7) / 8;
            Window<Bits> window = 0;
            for (size_t k = 0; k < window_bytes; ++k) {
                const size_t index = first_byte + k;
                const auto byte = index < available ? blocks[index] : std::uint8_t{0};
                window = static_cast<Window<Bits>>((window << 8U) | static_cast<Window<Bits>>(byte));
            }
            const unsigned drop = static_cast<unsigned>(window_bytes * 8 - shift - Bits);
            // The window still holds the bits of the previous value on its left.
            const Window<Bits> mask = static_cast<Window<Bits>>(
                    Bits >= 64 ? ~static_cast<std::uint64_t>(0) : ((std::uint64_t{1} << Bits) - 1));
            values[i] = static_cast<int64_t>(
                    static_cast<std::uint64_t>(static_cast<Window<Bits>>((window >> drop) & mask)));
        }
    }

    void encode(std::span<int64_t> values, size_t offset, std::span<uint8_t> blocks) const override {
        for (size_t i = 0; i < values.size(); ++i) {
            const size_t bit = (offset + i) * Bits;
            const size_t first_byte = bit >> 3;
            const unsigned shift = static_cast<unsigned>(bit & 7U);
            const size_t window_bytes = (shift + Bits + 7) / 8;
            const unsigned drop = static_cast<unsigned>(window_bytes * 8 - shift - Bits);
            const Window<Bits> mask = static_cast<Window<Bits>>(
                    Bits >= 64 ? ~static_cast<std::uint64_t>(0) : ((std::uint64_t{1} << Bits) - 1));
            // Read the window, clear the bits this value owns and put the value
            // there: the bits of the neighbouring values stay untouched, so a
            // caller can encode a slice of a stream.
            Window<Bits> window = 0;
            for (size_t k = 0; k < window_bytes; ++k) {
                const size_t index = first_byte + k;
                const auto byte = index < blocks.size() ? blocks[index] : std::uint8_t{0};
                window = static_cast<Window<Bits>>((window << 8U) | static_cast<Window<Bits>>(byte));
            }
            window = static_cast<Window<Bits>>(window & ~static_cast<Window<Bits>>(mask << drop));
            window = static_cast<Window<Bits>>(
                    window | static_cast<Window<Bits>>(static_cast<Window<Bits>>(
                                                               static_cast<std::uint64_t>(values[i]) & mask)
                                                       << drop));
            for (size_t k = window_bytes; k-- > 0;) {
                const size_t index = first_byte + k;
                if (index < blocks.size()) {
                    blocks[index] = static_cast<std::uint8_t>(window & 0xFFU);
                }
                window >>= 8U;
            }
        }
    }

    [[nodiscard]] size_t byte_count(size_t count) const noexcept override {
        return (count * Bits + 7) / 8;
    }

    [[nodiscard]] int32_t bits_per_value() const noexcept override { return static_cast<int32_t>(Bits); }
};

/// A function pointer per width, built once.
///
/// `std::array` of `unique_ptr` avoids the static-initialisation order problem a
/// table of references would have.
template <size_t... Widths>
std::array<std::unique_ptr<BulkOperation>, sizeof...(Widths)> make_table(std::index_sequence<Widths...>) {
    return {std::make_unique<PackedBulkOperation<Widths + 1>>()...};
}

const std::array<std::unique_ptr<BulkOperation>, 64> &table() {
    static const std::array<std::unique_ptr<BulkOperation>, 64> operations = make_table(std::make_index_sequence<64>{});
    return operations;
}

} // namespace

const BulkOperation &bulk_operation(int32_t bits_per_value) {
    if (bits_per_value < 1 || bits_per_value > 64) {
        throw std::invalid_argument("bits_per_value must be in [1, 64] (got " + std::to_string(bits_per_value) + ")");
    }
    return *table()[static_cast<size_t>(bits_per_value - 1)];
}

} // namespace lucene
