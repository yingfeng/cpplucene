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

export module lucene_core:bulk_operation;

import :types;
import std;

import :debug;
import :error;
import :packed_ints;

namespace lucene {

/// Bulk encoder/decoder for a packed-integer format.
///
/// Port of `BulkOperation` + `Decoder` + `Encoder`
/// (`core/util/packed/bulk_operation.rs`, `decoder.rs`, `encoder.rs`).
///
/// **Deviation:** the reference implementation splits the four bulk directions
/// into `Decoder` (read) and `Encoder` (write) traits that every operation
/// implements twice.  C++ has virtual dispatch, so this port merges them into one
/// interface; the method names and semantics are unchanged.
///
/// A bulk operation converts whole *blocks* of a packed stream at once:
///
///  * `iterations` blocks are read from `blocks` starting at `blocks_offset`;
///  * they expand into `iterations * <value count>` values written at
///    `values_offset`, or the other way round for the encode direction.
///
/// The `u64`/`u8` prefix names the block storage (64-bit words or raw bytes) and
/// the `i64`/`i32` suffix the value type.
export class BulkOperation {
public:
    virtual ~BulkOperation() = default;

    [[nodiscard]] virtual int32_t bits_per_value() const noexcept = 0;

    /// Number of 64-bit blocks that hold a whole number of values.
    [[nodiscard]] virtual size_t long_block_count() const noexcept = 0;
    /// Number of values in [`long_block_count`] blocks.
    [[nodiscard]] virtual size_t long_value_count() const noexcept = 0;
    /// Same as [`long_block_count`], in bytes.
    [[nodiscard]] virtual size_t byte_block_count() const noexcept = 0;
    /// Same as [`long_value_count`], for the byte layout.
    [[nodiscard]] virtual size_t byte_value_count() const noexcept = 0;

    // ---- decode ----

    virtual void decode_u64_to_i64(std::span<const uint64_t> blocks,
                                   size_t blocks_offset,
                                   std::span<int64_t> values,
                                   size_t values_offset,
                                   size_t iterations) const = 0;

    virtual void decode_u8_to_i64(std::span<const uint8_t> blocks,
                                  size_t blocks_offset,
                                  std::span<int64_t> values,
                                  size_t values_offset,
                                  size_t iterations) const = 0;

    /// Reports `unsupported_operation` for widths above 32 bits.
    [[nodiscard]] virtual VoidResult decode_u64_to_i32(std::span<const uint64_t> blocks,
                                                       size_t blocks_offset,
                                                       std::span<int32_t> values,
                                                       size_t values_offset,
                                                       size_t iterations) const = 0;

    /// Reports `unsupported_operation` for widths above 32 bits.
    [[nodiscard]] virtual VoidResult decode_u8_to_i32(std::span<const uint8_t> blocks,
                                                      size_t blocks_offset,
                                                      std::span<int32_t> values,
                                                      size_t values_offset,
                                                      size_t iterations) const = 0;

    // ---- encode ----

    virtual void encode_i64_to_u64(std::span<const int64_t> values,
                                   size_t values_offset,
                                   std::span<uint64_t> blocks,
                                   size_t blocks_offset,
                                   size_t iterations) const = 0;

    virtual void encode_i64_to_u8(std::span<const int64_t> values,
                                  size_t values_offset,
                                  std::span<uint8_t> blocks,
                                  size_t blocks_offset,
                                  size_t iterations) const = 0;

    virtual void encode_i32_to_u64(std::span<const int32_t> values,
                                   size_t values_offset,
                                   std::span<uint64_t> blocks,
                                   size_t blocks_offset,
                                   size_t iterations) const = 0;

    virtual void encode_i32_to_u8(std::span<const int32_t> values,
                                  size_t values_offset,
                                  std::span<uint8_t> blocks,
                                  size_t blocks_offset,
                                  size_t iterations) const = 0;

    // ---- helpers ----

    /// Writes `block` as eight big-endian bytes and returns the next offset.
    [[nodiscard]] static size_t write_long(uint64_t block, std::span<uint8_t> blocks, size_t blocks_offset);

    /// Number of iterations to perform so that `iterations * (block + 8 * value)`
    /// bytes stay within `ram_budget`.  Always at least one, and never more than
    /// the reader needs.
    [[nodiscard]] int32_t compute_iterations(int32_t value_count, int32_t ram_budget) const;
};

namespace detail {

[[nodiscard]] constexpr size_t odd_part(int32_t bits) noexcept {
    size_t blocks = static_cast<size_t>(bits);
    while ((blocks & 1) == 0) {
        blocks >>= 1;
    }
    return blocks;
}

[[nodiscard]] constexpr size_t packed_long_value_count(int32_t bits) noexcept {
    return 64 * odd_part(bits) / static_cast<size_t>(bits);
}

/// `(block_count, value_count)` reduced by their common power of two, i.e. the
/// byte layout of the same packing.
[[nodiscard]] constexpr std::pair<size_t, size_t> packed_byte_counts(int32_t bits) noexcept {
    size_t block_count = 8 * odd_part(bits);
    size_t value_count = packed_long_value_count(bits);
    while ((block_count & 1) == 0 && (value_count & 1) == 0) {
        block_count >>= 1;
        value_count >>= 1;
    }
    return {block_count, value_count};
}

/// `(1 << n) - 1`, saturating at 64 bits instead of being undefined behaviour.
[[nodiscard]] constexpr uint64_t low_mask(uint32_t n) noexcept {
    return n >= 64 ? ~uint64_t{0} : ((uint64_t{1} << n) - 1);
}

} // namespace detail

/// `Format.PACKED` bulk operation: values are laid out contiguously, most
/// significant bits first, exactly like
/// [`PackedFormat::kPacked`](:packed_ints).
///
/// The reference implementation ships one generated subclass per bit width
/// (1..24) plus a generic class for 25..64.  This port instantiates *one*
/// template for all 64 widths, so every generated subclass is covered by
/// constant folding instead of a runtime `bits_per_value`.
export template <int32_t BitsPerValue>
class BulkOperationPacked final : public BulkOperation {
    static_assert(BitsPerValue >= 1 && BitsPerValue <= 64, "bits per value must be in [1, 64]");

public:
    static constexpr int32_t kBits = BitsPerValue;
    static constexpr uint64_t kMask = detail::low_mask(static_cast<uint32_t>(BitsPerValue));
    static constexpr uint32_t kIntMask = static_cast<uint32_t>(kMask);
    static constexpr size_t kLongBlockCount = detail::odd_part(BitsPerValue);
    static constexpr size_t kLongValueCount = detail::packed_long_value_count(BitsPerValue);
    static constexpr size_t kByteBlockCount = detail::packed_byte_counts(BitsPerValue).first;
    static constexpr size_t kByteValueCount = detail::packed_byte_counts(BitsPerValue).second;

    [[nodiscard]] int32_t bits_per_value() const noexcept override { return kBits; }
    [[nodiscard]] size_t long_block_count() const noexcept override { return kLongBlockCount; }
    [[nodiscard]] size_t long_value_count() const noexcept override { return kLongValueCount; }
    [[nodiscard]] size_t byte_block_count() const noexcept override { return kByteBlockCount; }
    [[nodiscard]] size_t byte_value_count() const noexcept override { return kByteValueCount; }

    void decode_u64_to_i64(std::span<const uint64_t> blocks,
                           size_t blocks_offset,
                           std::span<int64_t> values,
                           size_t values_offset,
                           size_t iterations) const override {
        debug_check(blocks_offset + kLongBlockCount * iterations <= blocks.size(), "blocks overflow");
        debug_check(values_offset + kLongValueCount * iterations <= values.size(), "values overflow");
        int32_t bits_left = 64;
        const size_t total = kLongValueCount * iterations;
        for (size_t n = 0; n < total; ++n) {
            bits_left -= kBits;
            if (bits_left < 0) {
                const uint64_t lower = (blocks[blocks_offset] & detail::low_mask(static_cast<uint32_t>(kBits + bits_left)))
                        << static_cast<uint32_t>(-bits_left);
                ++blocks_offset;
                const uint64_t upper = blocks[blocks_offset] >> static_cast<uint32_t>(64 + bits_left);
                values[values_offset] = static_cast<int64_t>(lower | upper);
                bits_left += 64;
            } else {
                values[values_offset] = static_cast<int64_t>((blocks[blocks_offset] >> static_cast<uint32_t>(bits_left)) & kMask);
            }
            ++values_offset;
        }
    }

    void decode_u8_to_i64(std::span<const uint8_t> blocks,
                          size_t blocks_offset,
                          std::span<int64_t> values,
                          size_t values_offset,
                          size_t iterations) const override {
        decode_u8_into<int64_t>(blocks, blocks_offset, values, values_offset, iterations);
    }

    [[nodiscard]] VoidResult decode_u64_to_i32(std::span<const uint64_t> blocks,
                                              size_t blocks_offset,
                                              std::span<int32_t> values,
                                              size_t values_offset,
                                              size_t iterations) const override {
        if constexpr (kBits > 32) {
            return err(LuceneError::unsupported_operation(
                    "cannot decode " + std::to_string(kBits) + "-bit values into a 32-bit array"));
        } else {
            debug_check(blocks_offset + kLongBlockCount * iterations <= blocks.size(), "blocks overflow");
            debug_check(values_offset + kLongValueCount * iterations <= values.size(), "values overflow");
            int32_t bits_left = 64;
            const size_t total = kLongValueCount * iterations;
            for (size_t n = 0; n < total; ++n) {
                bits_left -= kBits;
                if (bits_left < 0) {
                    const uint64_t lower =
                            (blocks[blocks_offset] & detail::low_mask(static_cast<uint32_t>(kBits + bits_left)))
                            << static_cast<uint32_t>(-bits_left);
                    ++blocks_offset;
                    const uint64_t upper = blocks[blocks_offset] >> static_cast<uint32_t>(64 + bits_left);
                    values[values_offset] = static_cast<int32_t>(lower | upper);
                    bits_left += 64;
                } else {
                    values[values_offset] =
                            static_cast<int32_t>((blocks[blocks_offset] >> static_cast<uint32_t>(bits_left)) & kMask);
                }
                ++values_offset;
            }
            return {};
        }
    }

    [[nodiscard]] VoidResult decode_u8_to_i32(std::span<const uint8_t> blocks,
                                             size_t blocks_offset,
                                             std::span<int32_t> values,
                                             size_t values_offset,
                                             size_t iterations) const override {
        if constexpr (kBits > 32) {
            return err(LuceneError::unsupported_operation(
                    "cannot decode " + std::to_string(kBits) + "-bit values into a 32-bit array"));
        } else {
            decode_u8_into<int32_t>(blocks, blocks_offset, values, values_offset, iterations);
            return {};
        }
    }

    void encode_i64_to_u64(std::span<const int64_t> values,
                           size_t values_offset,
                           std::span<uint64_t> blocks,
                           size_t blocks_offset,
                           size_t iterations) const override {
        debug_check(values_offset + kLongValueCount * iterations <= values.size(), "values overflow");
        debug_check(blocks_offset + kLongBlockCount * iterations <= blocks.size(), "blocks overflow");
        uint64_t next_block = 0;
        int32_t bits_left = 64;
        const size_t total = kLongValueCount * iterations;
        for (size_t n = 0; n < total; ++n) {
            bits_left -= kBits;
            if (bits_left > 0) {
                next_block |= static_cast<uint64_t>(values[values_offset]) << static_cast<uint32_t>(bits_left);
                ++values_offset;
            } else if (bits_left == 0) {
                next_block |= static_cast<uint64_t>(values[values_offset]);
                ++values_offset;
                blocks[blocks_offset] = next_block;
                ++blocks_offset;
                next_block = 0;
                bits_left = 64;
            } else {
                next_block |= static_cast<uint64_t>(values[values_offset]) >> static_cast<uint32_t>(-bits_left);
                blocks[blocks_offset] = next_block;
                ++blocks_offset;
                next_block = (static_cast<uint64_t>(values[values_offset]) & detail::low_mask(static_cast<uint32_t>(-bits_left)))
                        << static_cast<uint32_t>(64 + bits_left);
                ++values_offset;
                bits_left += 64;
            }
        }
    }

    void encode_i64_to_u8(std::span<const int64_t> values,
                          size_t values_offset,
                          std::span<uint8_t> blocks,
                          size_t blocks_offset,
                          size_t iterations) const override {
        debug_check(values_offset + kByteValueCount * iterations <= values.size(), "values overflow");
        debug_check(blocks_offset + kByteBlockCount * iterations <= blocks.size(), "blocks overflow");
        int32_t next_block = 0;
        int32_t bits_left = 8;
        const size_t total = kByteValueCount * iterations;
        for (size_t n = 0; n < total; ++n) {
            const int64_t value = values[values_offset];
            ++values_offset;
            debug_check(kBits >= PackedInts::unsigned_bits_required(value), "value needs more bits than bits_per_value");
            if (kBits < bits_left) {
                next_block |= static_cast<int32_t>(value << (bits_left - kBits));
                bits_left -= kBits;
            } else {
                int32_t bits = kBits - bits_left;
                blocks[blocks_offset] = static_cast<uint8_t>(static_cast<uint64_t>(next_block) |
                                                             (static_cast<uint64_t>(value) >> static_cast<uint32_t>(bits)));
                ++blocks_offset;
                while (bits >= 8) {
                    bits -= 8;
                    blocks[blocks_offset] = static_cast<uint8_t>(static_cast<uint64_t>(value) >> static_cast<uint32_t>(bits));
                    ++blocks_offset;
                }
                bits_left = 8 - bits;
                next_block = static_cast<int32_t>((static_cast<uint64_t>(value) & detail::low_mask(static_cast<uint32_t>(bits)))
                                                  << static_cast<uint32_t>(bits_left));
            }
        }
        debug_check(bits_left == 8, "the byte layout must end on a byte boundary");
    }

    void encode_i32_to_u64(std::span<const int32_t> values,
                           size_t values_offset,
                           std::span<uint64_t> blocks,
                           size_t blocks_offset,
                           size_t iterations) const override {
        debug_check(values_offset + kLongValueCount * iterations <= values.size(), "values overflow");
        debug_check(blocks_offset + kLongBlockCount * iterations <= blocks.size(), "blocks overflow");
        uint64_t next_block = 0;
        int32_t bits_left = 64;
        const size_t total = kLongValueCount * iterations;
        for (size_t n = 0; n < total; ++n) {
            bits_left -= kBits;
            if (bits_left > 0) {
                next_block |= (static_cast<uint64_t>(values[values_offset]) & 0xFFFFFFFFULL) << static_cast<uint32_t>(bits_left);
                ++values_offset;
            } else if (bits_left == 0) {
                next_block |= static_cast<uint64_t>(values[values_offset]) & 0xFFFFFFFFULL;
                ++values_offset;
                blocks[blocks_offset] = next_block;
                ++blocks_offset;
                next_block = 0;
                bits_left = 64;
            } else {
                next_block |= (static_cast<uint64_t>(values[values_offset]) & 0xFFFFFFFFULL) >> static_cast<uint32_t>(-bits_left);
                blocks[blocks_offset] = next_block;
                ++blocks_offset;
                next_block = (static_cast<uint64_t>(values[values_offset]) & detail::low_mask(static_cast<uint32_t>(-bits_left)))
                        << static_cast<uint32_t>(64 + bits_left);
                ++values_offset;
                bits_left += 64;
            }
        }
    }

    void encode_i32_to_u8(std::span<const int32_t> values,
                          size_t values_offset,
                          std::span<uint8_t> blocks,
                          size_t blocks_offset,
                          size_t iterations) const override {
        debug_check(values_offset + kByteValueCount * iterations <= values.size(), "values overflow");
        debug_check(blocks_offset + kByteBlockCount * iterations <= blocks.size(), "blocks overflow");
        int32_t next_block = 0;
        int32_t bits_left = 8;
        const size_t total = kByteValueCount * iterations;
        for (size_t n = 0; n < total; ++n) {
            const int32_t value = values[values_offset];
            ++values_offset;
            debug_check(kBits >= PackedInts::unsigned_bits_required(static_cast<int64_t>(static_cast<uint64_t>(value) & 0xFFFFFFFFULL)),
                        "value needs more bits than bits_per_value");
            if (kBits < bits_left) {
                next_block |= value << (bits_left - kBits);
                bits_left -= kBits;
            } else {
                int32_t bits = kBits - bits_left;
                blocks[blocks_offset] =
                        static_cast<uint8_t>(static_cast<uint32_t>(next_block) | (static_cast<uint32_t>(value) >> static_cast<uint32_t>(bits)));
                ++blocks_offset;
                while (bits >= 8) {
                    bits -= 8;
                    blocks[blocks_offset] = static_cast<uint8_t>(static_cast<uint32_t>(value) >> static_cast<uint32_t>(bits));
                    ++blocks_offset;
                }
                bits_left = 8 - bits;
                next_block = static_cast<int32_t>((static_cast<uint32_t>(value) & static_cast<uint32_t>(detail::low_mask(static_cast<uint32_t>(bits))))
                                                  << static_cast<uint32_t>(bits_left));
            }
        }
        debug_check(bits_left == 8, "the byte layout must end on a byte boundary");
    }

private:
    /// One big-endian 64-bit word, folded from `memcpy` so it compiles to a
    /// single unaligned load plus a `bswap` on little-endian hosts.
    [[nodiscard]] static uint64_t read_be64(const uint8_t *p) noexcept {
        uint64_t word = 0;
        std::memcpy(&word, p, sizeof(word));
        if constexpr (std::endian::native == std::endian::little) {
            word = ((word & 0x00000000000000FFULL) << 56) | ((word & 0x000000000000FF00ULL) << 40) |
                   ((word & 0x0000000000FF0000ULL) << 24) | ((word & 0x00000000FF000000ULL) << 8) |
                   ((word & 0x000000FF00000000ULL) >> 8) | ((word & 0x0000FF0000000000ULL) >> 24) |
                   ((word & 0x00FF000000000000ULL) >> 40) | ((word & 0xFF00000000000000ULL) >> 56);
        }
        return word;
    }

    /// Shared body of `decode_u8_to_i64` / `decode_u8_to_i32`.
    ///
    /// The reference implementation folds the byte stream through a
    /// byte-at-a-time state machine, which costs a branch per byte.  This port
    /// uses two kernels instead:
    ///
    ///  * widths that are a multiple of eight are whole bytes per value, so the
    ///    values are assembled with a plain byte loop (the compiler vectorises
    ///    it);
    ///  * other widths go through a 128-bit bit buffer fed by 64-bit
    ///    big-endian loads, so one load covers `64 / kBits` values and a value
    ///    that straddles a word boundary needs no special case.  At most 127
    ///    bits are live: a refill only runs while fewer than `kBits` (<= 64)
    ///    bits are buffered.
    template <typename Value>
    static void decode_u8_into(std::span<const uint8_t> blocks,
                               size_t blocks_offset,
                               std::span<Value> values,
                               size_t values_offset,
                               size_t iterations) {
        debug_check(blocks_offset + kByteBlockCount * iterations <= blocks.size(), "blocks overflow");
        debug_check(values_offset + kByteValueCount * iterations <= values.size(), "values overflow");
        const uint8_t *const data = blocks.data() + blocks_offset;

        if constexpr (kBits % 8 == 0) {
            constexpr size_t kBytesPerValue = static_cast<size_t>(kBits) / 8;
            const size_t total_values = kByteValueCount * iterations;
            const uint8_t *cursor = data;
            for (size_t n = 0; n < total_values; ++n) {
                uint64_t value = 0;
                for (size_t k = 0; k < kBytesPerValue; ++k) {
                    value = (value << 8U) | *cursor;
                    ++cursor;
                }
                values[values_offset] = static_cast<Value>(value);
                ++values_offset;
            }
            return;
        }

        // `kByteBlockCount` bytes hold exactly `kByteValueCount` values, so the
        // buffer is drained at the end of every group.
        unsigned __int128 acc = 0;
        uint32_t acc_bits = 0;
        for (size_t group = 0; group < iterations; ++group) {
            const uint8_t *group_start = data + group * kByteBlockCount;
            size_t consumed = 0;
            for (size_t i = 0; i < kByteValueCount; ++i) {
                while (acc_bits < static_cast<uint32_t>(kBits)) {
                    const size_t available = kByteBlockCount - consumed;
                    if (available >= sizeof(uint64_t)) {
                        acc = (acc << 64U) | read_be64(group_start + consumed);
                        consumed += sizeof(uint64_t);
                        acc_bits += 64;
                    } else {
                        uint64_t tail = 0;
                        for (size_t k = 0; k < available; ++k) {
                            tail = (tail << 8U) | group_start[consumed + k];
                        }
                        consumed += available;
                        acc = (acc << static_cast<uint32_t>(available * 8)) | tail;
                        acc_bits += static_cast<uint32_t>(available * 8);
                    }
                }
                const uint32_t shift = acc_bits - static_cast<uint32_t>(kBits);
                values[values_offset] = static_cast<Value>(static_cast<uint64_t>(acc >> shift) & kMask);
                ++values_offset;
                acc_bits = shift;
            }
        }
        debug_check(acc_bits == 0, "the byte layout must end on a byte boundary");
    }
};

/// Bulk operation for the deprecated `PACKED_SINGLE_BLOCK` layout: one 64-bit
/// block holds `64 / bits_per_value` values, least significant first.
///
/// The reference implementation has a single non-specialized class here as well.
export class BulkOperationPackedSingleBlock final : public BulkOperation {
public:
    explicit constexpr BulkOperationPackedSingleBlock(int32_t bits_per_value) noexcept
            : bits_per_value_(bits_per_value),
              value_count_(static_cast<size_t>(64 / bits_per_value)),
              mask_(detail::low_mask(static_cast<uint32_t>(bits_per_value))) {
        debug_check(bits_per_value >= 1 && bits_per_value <= 32, "bits per value must be in [1, 32]");
    }

    [[nodiscard]] int32_t bits_per_value() const noexcept override { return bits_per_value_; }
    [[nodiscard]] size_t long_block_count() const noexcept override { return 1; }
    [[nodiscard]] size_t long_value_count() const noexcept override { return value_count_; }
    [[nodiscard]] size_t byte_block_count() const noexcept override { return 8; }
    [[nodiscard]] size_t byte_value_count() const noexcept override { return value_count_; }

    void decode_u64_to_i64(std::span<const uint64_t> blocks,
                           size_t blocks_offset,
                           std::span<int64_t> values,
                           size_t values_offset,
                           size_t iterations) const override {
        for (size_t n = 0; n < iterations; ++n) {
            uint64_t block = blocks[blocks_offset];
            ++blocks_offset;
            values_offset = decode_block_to_i64(block, values, values_offset);
        }
    }

    void decode_u8_to_i64(std::span<const uint8_t> blocks,
                          size_t blocks_offset,
                          std::span<int64_t> values,
                          size_t values_offset,
                          size_t iterations) const override {
        for (size_t n = 0; n < iterations; ++n) {
            const uint64_t block = read_long(blocks, blocks_offset);
            blocks_offset += 8;
            values_offset = decode_block_to_i64(block, values, values_offset);
        }
    }

    [[nodiscard]] VoidResult decode_u64_to_i32(std::span<const uint64_t> blocks,
                                              size_t blocks_offset,
                                              std::span<int32_t> values,
                                              size_t values_offset,
                                              size_t iterations) const override {
        if (bits_per_value_ > 32) {
            return err(LuceneError::unsupported_operation(
                    "cannot decode " + std::to_string(bits_per_value_) + "-bit values into a 32-bit array"));
        }
        for (size_t n = 0; n < iterations; ++n) {
            uint64_t block = blocks[blocks_offset];
            ++blocks_offset;
            values_offset = decode_block_to_i32(block, values, values_offset);
        }
        return {};
    }

    [[nodiscard]] VoidResult decode_u8_to_i32(std::span<const uint8_t> blocks,
                                             size_t blocks_offset,
                                             std::span<int32_t> values,
                                             size_t values_offset,
                                             size_t iterations) const override {
        if (bits_per_value_ > 32) {
            return err(LuceneError::unsupported_operation(
                    "cannot decode " + std::to_string(bits_per_value_) + "-bit values into a 32-bit array"));
        }
        for (size_t n = 0; n < iterations; ++n) {
            const uint64_t block = read_long(blocks, blocks_offset);
            blocks_offset += 8;
            values_offset = decode_block_to_i32(block, values, values_offset);
        }
        return {};
    }

    void encode_i64_to_u64(std::span<const int64_t> values,
                           size_t values_offset,
                           std::span<uint64_t> blocks,
                           size_t blocks_offset,
                           size_t iterations) const override {
        for (size_t n = 0; n < iterations; ++n) {
            blocks[blocks_offset] = encode_block_from_i64(values, values_offset);
            ++blocks_offset;
            values_offset += value_count_;
        }
    }

    void encode_i64_to_u8(std::span<const int64_t> values,
                          size_t values_offset,
                          std::span<uint8_t> blocks,
                          size_t blocks_offset,
                          size_t iterations) const override {
        for (size_t n = 0; n < iterations; ++n) {
            const uint64_t block = encode_block_from_i64(values, values_offset);
            values_offset += value_count_;
            blocks_offset = write_long(block, blocks, blocks_offset);
        }
    }

    void encode_i32_to_u64(std::span<const int32_t> values,
                           size_t values_offset,
                           std::span<uint64_t> blocks,
                           size_t blocks_offset,
                           size_t iterations) const override {
        for (size_t n = 0; n < iterations; ++n) {
            blocks[blocks_offset] = encode_block_from_i32(values, values_offset);
            ++blocks_offset;
            values_offset += value_count_;
        }
    }

    void encode_i32_to_u8(std::span<const int32_t> values,
                          size_t values_offset,
                          std::span<uint8_t> blocks,
                          size_t blocks_offset,
                          size_t iterations) const override {
        for (size_t n = 0; n < iterations; ++n) {
            const uint64_t block = encode_block_from_i32(values, values_offset);
            values_offset += value_count_;
            blocks_offset = write_long(block, blocks, blocks_offset);
        }
    }

    /// Decodes one block into `values`, returning the next value offset.
    [[nodiscard]] size_t decode_block_to_i64(uint64_t block,
                                             std::span<int64_t> values,
                                             size_t values_offset) const noexcept {
        values[values_offset] = static_cast<int64_t>(block & mask_);
        ++values_offset;
        for (size_t n = 1; n < value_count_; ++n) {
            block >>= static_cast<uint32_t>(bits_per_value_);
            values[values_offset] = static_cast<int64_t>(block & mask_);
            ++values_offset;
        }
        return values_offset;
    }

    /// Decodes one block into `values`, returning the next value offset.
    [[nodiscard]] size_t decode_block_to_i32(uint64_t block,
                                             std::span<int32_t> values,
                                             size_t values_offset) const noexcept {
        values[values_offset] = static_cast<int32_t>(block & mask_);
        ++values_offset;
        for (size_t n = 1; n < value_count_; ++n) {
            block >>= static_cast<uint32_t>(bits_per_value_);
            values[values_offset] = static_cast<int32_t>(block & mask_);
            ++values_offset;
        }
        return values_offset;
    }

    [[nodiscard]] uint64_t encode_block_from_i64(std::span<const int64_t> values, size_t values_offset) const noexcept {
        uint64_t block = static_cast<uint64_t>(values[values_offset]);
        ++values_offset;
        for (size_t j = 1; j < value_count_; ++j) {
            block |= static_cast<uint64_t>(values[values_offset]) << static_cast<uint32_t>(j * static_cast<size_t>(bits_per_value_));
            ++values_offset;
        }
        return block;
    }

    [[nodiscard]] uint64_t encode_block_from_i32(std::span<const int32_t> values, size_t values_offset) const noexcept {
        uint64_t block = static_cast<uint64_t>(values[values_offset]) & 0xFFFFFFFFULL;
        ++values_offset;
        for (size_t j = 1; j < value_count_; ++j) {
            block |= (static_cast<uint64_t>(values[values_offset]) & 0xFFFFFFFFULL)
                    << static_cast<uint32_t>(j * static_cast<size_t>(bits_per_value_));
            ++values_offset;
        }
        return block;
    }

    [[nodiscard]] static uint64_t read_long(std::span<const uint8_t> blocks, size_t blocks_offset) noexcept {
        uint64_t value = 0;
        for (size_t i = 0; i < 8; ++i) {
            value = (value << 8) | static_cast<uint64_t>(blocks[blocks_offset + i]);
        }
        return value;
    }

private:
    int32_t bits_per_value_;
    size_t value_count_;
    uint64_t mask_;
};

/// The bulk operation for `format` at `bits_per_value`.
///
/// Returns a reference to a process-wide instance, like the reference
/// implementation's `PACKED_BULK_OPS` / `PACKED_SINGLE_BLOCK_BULK_OPS` tables.
export [[nodiscard]] Result<const BulkOperation *> bulk_operation_of(PackedFormat format, int32_t bits_per_value);

/// A [`BulkOperation`] applied to a whole byte stream.
///
/// The bulk interface works in block groups, because that is the only unit a
/// packed stream can be split on.  A stream, however, has an arbitrary value
/// count, so the last (partial) group is decoded through a scratch buffer: only
/// the bytes that the values actually occupy are read or written, i.e. the
/// stream stays `ceil(count * bits / 8)` bytes long.
export class PackedBulkStream {
public:
    explicit PackedBulkStream(const BulkOperation &operation) noexcept : operation_(operation) {}

    /// Decodes `values.size()` values starting at `blocks[blocks_offset]`.
    [[nodiscard]] VoidResult decode(std::span<const uint8_t> blocks, size_t blocks_offset,
                                    std::span<int64_t> values) const;

    /// Encodes `values` (starting at `values_offset`) into `blocks[blocks_offset]`.
    [[nodiscard]] VoidResult encode(std::span<const int64_t> values, size_t values_offset,
                                    std::span<uint8_t> blocks, size_t blocks_offset = 0) const;

    [[nodiscard]] const BulkOperation &operation() const noexcept { return operation_; }

private:
    const BulkOperation &operation_;
};

/// The `Format.PACKED` operation for `bits_per_value`, wrapped as a stream codec.
export [[nodiscard]] Result<PackedBulkStream> bulk_operation(int32_t bits_per_value);

} // namespace lucene
