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

export module lucene_core:packed_ints;

import :types;
import :error;

import std;

namespace lucene {

/// Constants and helpers shared by every packed-integer codec.
///
/// Port of `util/packed/packed_ints.rs` (Lucene's `PackedInts`).
export struct PackedInts {
    /// Acceptable overhead ratio for the fastest (but least compact) format.
    static constexpr float FASTEST = 7.0F;
    static constexpr float FAST = 0.5F;
    static constexpr float DEFAULT = 0.25F;
    /// No overhead tolerated at all: the most compact format.
    static constexpr float COMPACT = 0.0F;

    /// Buffer size hint used by the writers and readers.
    static constexpr int32_t DEFAULT_BUFFER_SIZE = 1024;

    static constexpr std::string_view CODEC_NAME = "PackedInts";

    static constexpr int32_t VERSION_MONOTONIC_WITHOUT_ZIGZAG = 2;
    static constexpr int32_t VERSION_START = VERSION_MONOTONIC_WITHOUT_ZIGZAG;
    static constexpr int32_t VERSION_CURRENT = VERSION_MONOTONIC_WITHOUT_ZIGZAG;

    /// Bits required to represent values from `0` to `max_value`.
    [[nodiscard]] static Result<int32_t> bits_required(int64_t max_value);

    /// Bits required to store `bits` interpreted as an unsigned value; at least 1.
    [[nodiscard]] static constexpr int32_t unsigned_bits_required(int64_t bits) noexcept {
        const int32_t num_bits = 64 - static_cast<int32_t>(std::countl_zero(static_cast<uint64_t>(bits)));
        return num_bits < 1 ? 1 : num_bits;
    }

    /// Largest unsigned value representable with `bits_per_value` bits.
    [[nodiscard]] static constexpr int64_t max_value(int32_t bits_per_value) noexcept {
        return bits_per_value >= 64 ? std::numeric_limits<int64_t>::max()
                                    : static_cast<int64_t>((uint64_t{1} << bits_per_value) - 1);
    }

    /// Validates a `PackedInts` version.
    [[nodiscard]] static VoidResult check_version(int32_t version);

    /// `ceil(size / block_size)` with an argument check.
    [[nodiscard]] static Result<size_t> num_blocks(size_t size, size_t block_size);
};

/// Bit layout of a packed sequence.  Lucene's `PackedInts.Format`.
export enum class PackedFormat : uint8_t {
    /// All bits are written contiguously: `ceil(value_count * bits / 8)` bytes.
    kPacked = 0,
    /// Deprecated layout that pads every value group to a whole 64-bit block.
    kPackedSingleBlock = 1,
};

/// Number of bytes [`PackedFormat::kPacked`] needs for `value_count` values.
[[nodiscard]] size_t packed_byte_count(int32_t version, int64_t value_count, int32_t bits_per_value);

/// Number of bytes [`PackedFormat::kPackedSingleBlock`] needs.
///
/// Only defined for `bits_per_value` in `{1, 2, 4, 8, 16, 32, 64}`, where one
/// 64-bit block holds `64 / bits_per_value` values.
[[nodiscard]] Result<size_t> packed_single_block_byte_count(int32_t version, int64_t value_count,
                                                            int32_t bits_per_value);

/// A [`PackedFormat`] together with the number of bits it uses per value.
export struct FormatAndBits {
    PackedFormat format{PackedFormat::kPacked};
    int32_t bits_per_value{1};
};

/// Picks the format and bit width whose overhead stays within
/// `acceptable_overhead_ratio` (`PackedInts.fastestFormatAndBits`).
///
/// `value_count` is unused: it is part of the Java signature only, and rlucene
/// keeps it as a `TODO` placeholder as well.
[[nodiscard]] FormatAndBits fastest_format_and_bits(int32_t value_count, int32_t bits_per_value,
                                                    float acceptable_overhead_ratio);

/// Random access to packed values.  Lucene's `PackedInts.Reader`.
export class PackedIntsReader {
public:
    virtual ~PackedIntsReader() = default;

    [[nodiscard]] virtual int64_t get(size_t index) const = 0;
    [[nodiscard]] virtual size_t size() const = 0;
    [[nodiscard]] virtual int32_t bits_per_value() const = 0;
};

/// Sequential writer of packed values.  Lucene's `PackedInts.Writer`.
export class PackedIntsWriter {
public:
    virtual ~PackedIntsWriter() = default;

    [[nodiscard]] virtual VoidResult add(int64_t value) = 0;
    [[nodiscard]] virtual VoidResult finish() = 0;
    [[nodiscard]] virtual int32_t bits_per_value() const = 0;
    [[nodiscard]] virtual int64_t ordinal() const = 0;
};

/// A reader that always returns 0 (`PackedInts.NullReader`).
export class NullPackedReader final : public PackedIntsReader {
public:
    explicit NullPackedReader(size_t value_count) : value_count_(value_count) {}

    [[nodiscard]] int64_t get(size_t index) const override {
        (void)index;
        return 0;
    }
    [[nodiscard]] size_t size() const override { return value_count_; }
    [[nodiscard]] int32_t bits_per_value() const override { return 0; }

private:
    size_t value_count_;
};

} // namespace lucene
