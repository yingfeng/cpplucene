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

export module lucene_core:lz4;

import :types;
import :error;

import std;

import :data_input;
import :data_output;

namespace lucene {

/// A record of previous occurrences of 4-byte sequences, used by [`LZ4`].
///
/// Port of the `HashTable` trait (`util/compress/lz4.rs`).  Instances must not
/// be shared across threads but can be reused for many `compress` calls.
export class Lz4HashTable {
public:
    virtual ~Lz4HashTable() = default;

    /// Resets the table in order to compress `[off, off + len)`.
    virtual void reset(int32_t off, int32_t len) = 0;

    /// Registers `dict_len` bytes starting at the reset offset as a dictionary.
    virtual void init_dictionary(int32_t dict_len, std::span<const uint8_t> bytes) = 0;

    /// Advances the cursor to `off` and returns an earlier index holding the
    /// same 4 bytes.  May only be called with strictly increasing offsets.
    [[nodiscard]] virtual Result<std::optional<int32_t>> get(int32_t off, std::span<const uint8_t> bytes) = 0;

    /// Returns an earlier index holding the same 4 bytes; unlike [`get`] it
    /// does not require increasing offsets.
    [[nodiscard]] virtual std::optional<int32_t> previous(int32_t off, std::span<const uint8_t> bytes) = 0;
};

/// Lossy [`Lz4HashTable`] that only remembers the last occurrence of every
/// hash, in about 16kB of memory.  This is the table used for all inputs whose
/// size is at most 64kB.
///
/// **Deviation:** rlucene stores 16-bit offsets in a `Vec<u16>`; here one
/// `int32_t` slot is used for both the 16- and 32-bit variants, trading 2 bytes
/// per slot for a single code path.
export class FastCompressionHashTable final : public Lz4HashTable {
public:
    void reset(int32_t off, int32_t len) override;
    void init_dictionary(int32_t dict_len, std::span<const uint8_t> bytes) override;
    [[nodiscard]] Result<std::optional<int32_t>> get(int32_t off, std::span<const uint8_t> bytes) override;
    [[nodiscard]] std::optional<int32_t> previous(int32_t off, std::span<const uint8_t> bytes) override;

private:
    int32_t base_{0};
    int32_t last_off_{0};
    int32_t end_{0};
    int32_t hash_log_{0};
    int32_t bits_per_value_{0};
    std::vector<int32_t> table_{};
};

/// High-precision [`Lz4HashTable`]: remembers up to 256 occurrences of 4-byte
/// sequences within the last 64kB through a chain table, which makes matches
/// much more likely than [`FastCompressionHashTable`].
export class HighCompressionHashTable final : public Lz4HashTable {
public:
    static constexpr int32_t MAX_ATTEMPTS = 256;
    /// `LZ4::MAX_DISTANCE - 1`.
    static constexpr int32_t MASK = (1 << 16) - 1;

    void reset(int32_t off, int32_t len) override;
    void init_dictionary(int32_t dict_len, std::span<const uint8_t> bytes) override;
    [[nodiscard]] Result<std::optional<int32_t>> get(int32_t off, std::span<const uint8_t> bytes) override;
    [[nodiscard]] std::optional<int32_t> previous(int32_t off, std::span<const uint8_t> bytes) override;

private:
    void add_hash(int32_t off, std::span<const uint8_t> bytes);

    int32_t base_{0};
    int32_t next_{0};
    int32_t end_{0};
    int32_t attempts_{0};
    std::vector<int32_t> hash_table_{};
    std::vector<uint16_t> chain_table_{};
};

/// LZ4 compression and decompression.
///
/// Port of `util/compress/lz4.rs`, itself a port of Lucene's
/// `compression/LZ4`.  The high-compression mode is a simplified variant of the
/// original algorithm: it only keeps a better hash table that remembers more
/// occurrences of a 4-byte sequence and drops the overlapping-match logic.
export class LZ4 {
public:
    /// Maximum supported distance between two sequences that can be replaced by
    /// a back reference.
    static constexpr int32_t MAX_DISTANCE = 1 << 16;
    static constexpr int32_t MEMORY_USAGE = 14;
    /// Minimum length of a match.
    static constexpr int32_t MIN_MATCH = 4;
    /// The last bytes of the input must always be encoded as literals.
    static constexpr int32_t LAST_LITERALS = 5;
    static constexpr int32_t HASH_LOG_HC = 15;
    static constexpr int32_t HASH_TABLE_SIZE_HC = 1 << HASH_LOG_HC;

    /// Decompresses at least `decompressed_len` bytes into `dest[d_off..]`.
    ///
    /// `dest` must be large enough to hold **all** decompressed data.  When the
    /// data was compressed with a preset dictionary, the same dictionary must be
    /// present in `dest[d_off - dict_len..d_off]`.  Returns the new offset.
    [[nodiscard]] static Result<int32_t> decompress(DataInput &compressed, int32_t decompressed_len,
                                                    std::span<uint8_t> dest, int32_t d_off);

    /// Compresses `bytes[off..off+len]` into `out`.
    [[nodiscard]] static VoidResult compress(std::span<const uint8_t> bytes, int32_t off, int32_t len,
                                             DataOutput &out, Lz4HashTable &ht);

    /// Compresses `[dict_off + dict_len, dict_off + dict_len + len)` using
    /// `[dict_off, dict_off + dict_len)` as a dictionary.  `dict_len` must not
    /// exceed [`MAX_DISTANCE`], the maximum window size.
    [[nodiscard]] static VoidResult compress_with_dictionary(std::span<const uint8_t> bytes, int32_t dict_off,
                                                             int32_t dict_len, int32_t len, DataOutput &out,
                                                             Lz4HashTable &ht);
};

} // namespace lucene
