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

export module lucene_core:utf32_to_utf8;

import :types;
import std;

import :automaton;
import :error;

namespace lucene {

/// One of the one to four UTF-8 bytes that define a code point: `value` is the
/// byte value, `bits` how many bits UTF-8 uses at that byte.
export struct UTF8Byte {
    uint8_t value{0};
    uint8_t bits{0};
};

/// A single code point, as a sequence of one to four UTF-8 bytes.
export class UTF8Sequence {
public:
    [[nodiscard]] int32_t byte_at(size_t idx) const noexcept { return bytes_[idx].value; }
    [[nodiscard]] size_t num_bits(size_t idx) const noexcept { return bytes_[idx].bits; }
    /// Sets every byte of `code`, and the length of the sequence.
    void set(int32_t code);
    /// Sets only the number of bytes and the first one, for the scratch
    /// sequences that bracket a range of code lengths.
    void set_first_byte(int32_t code);

    size_t len{0};

private:
    void set_rest(int32_t code, size_t num_bytes);

    std::array<UTF8Byte, 4> bytes_{};
};

/// Converts UTF-32 automata to the equivalent UTF-8 representation.
///
/// Port of `UTF32ToUTF8` (`core/util/automation/utf32_to_utf8.rs`).
///
/// **Deviations:** the conversion state is local to [`convert`] instead of being
/// stored in the converter, and `Cow` becomes `std::optional` (`nullopt` means
/// "use the input", which is what happens for an automaton with no states).
export class UTF32ToUTF8 {
public:
    /// Converts `utf32` to an equivalent UTF-8 automaton.  The result is not
    /// generally deterministic, so determinize it when that is required.
    [[nodiscard]] static Result<std::optional<Automaton>> convert(const Automaton &utf32);

    /// The first code point of each UTF-8 length: 0, 0x80, 0x800, 0x10000.
    static constexpr std::array<int32_t, 4> START_CODES{0, 128, 2048, 65536};
    /// The last code point of each UTF-8 length: 0x7F, 0x7FF, 0xFFFF, 0x10FFFF.
    static constexpr std::array<int32_t, 4> END_CODES{127, 2047, 65535, 0x10FFFF};
    /// `MASKS[i]` has its low `i` bits set, for `i` in `[0, 7]`.
    static constexpr std::array<uint8_t, 8> MASKS{0, 1, 3, 7, 15, 31, 63, 127};
};

} // namespace lucene
