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

export module lucene_core:compression;

import :types;
import std;

import :close;
import :data_input;
import :data_output;
import :error;

namespace lucene {

/// A data compressor.
///
/// Port of the `Compressor` trait (`codecs/compression/compressor.rs`).  The
/// compressor must add everything a [`Decompressor`] needs to know where the
/// compressed data ends.
export class Compressor : public Closeable {
public:
    ~Compressor() override = default;

    /// Compresses `data` into `out`.
    [[nodiscard]] virtual VoidResult compress(std::span<const uint8_t> data, DataOutput &out) = 0;
};

/// A data decompressor.
///
/// Port of the `Decompressor` trait (`codecs/compression/decompressor.rs`).
/// Decompressors are stateful (they remember how much of a chunk they have
/// already produced) and are cloned per reader, which is why [`clone`] is part
/// of the interface instead of a copy constructor.
export class Decompressor {
public:
    virtual ~Decompressor() = default;

    /// Decompresses `length` bytes of the chunk of `original_length` bytes into
    /// `compressed[offset, offset + length)`.
    ///
    /// `dest` is grown to at least `original_length + 7` bytes; only
    /// `dest[offset, offset + length)` is guaranteed to hold the result, because
    /// a compression format that cannot seek has to reproduce everything before
    /// `offset`.
    [[nodiscard]] virtual VoidResult decompress(DataInput &compressed, int32_t original_length, int32_t offset,
                                                int32_t length, std::vector<uint8_t> &dest) = 0;

    /// A decompressor with the same configuration and no state.
    [[nodiscard]] virtual std::unique_ptr<Decompressor> clone() const = 0;
};

/// Which compression algorithm a format uses.
export enum class CompressionModeType : uint8_t {
    /// Trades compression ratio for speed: fast to read and fast to write.
    Fast = 0,
    /// Trades write speed for a better compression ratio, but still fast to read.
    FastDecompression = 1,
    /// LZ4 with a preset dictionary: one dictionary block followed by sub blocks
    /// that may reference it.  This is Lucene's `BEST_SPEED`, the default for the
    /// stored fields and term vectors formats.
    BestSpeed = 2,
    /// Deflate with a preset dictionary: much better compression at the cost of
    /// speed.  This is Lucene's `HIGH_COMPRESSION`.
    HighCompression = 3,
};

/// A compression mode.
///
/// Port of `CompressionMode`
/// (`codecs/compression/compression_mode.rs`).  Both modes are LZ4: `Fast` picks
/// matches greedily, `FastDecompression` searches harder while writing.
///
/// All three reference modes are ported: the two LZ4 modes, `BEST_SPEED` (LZ4
/// with a preset dictionary) and `HIGH_COMPRESSION` (Deflate with a preset
/// dictionary, see
/// [`DeflateWithPresetDictCompressor`](lucene_core:deflate_with_preset_dict_compression_mode)).
export class CompressionMode {
public:
    CompressionMode() = default;
    explicit CompressionMode(CompressionModeType type) : type_(type) {}

    /// LZ4 with a greedy match search.
    [[nodiscard]] static CompressionMode fast() { return CompressionMode(CompressionModeType::Fast); }

    /// LZ4 with a deeper match search; the decompressor is the same as `fast`.
    [[nodiscard]] static CompressionMode fast_decompression() {
        return CompressionMode(CompressionModeType::FastDecompression);
    }

    /// LZ4 with a preset dictionary, Lucene's `BEST_SPEED`.
    ///
    /// Port of `LZ4WithPresetDictCompressionMode`: the chunk is split into a
    /// dictionary block and ten sub blocks, and every sub block is compressed
    /// against that shared dictionary.
    [[nodiscard]] static CompressionMode best_speed() {
        return CompressionMode(CompressionModeType::BestSpeed);
    }

    /// Deflate with a preset dictionary, Lucene's `HIGH_COMPRESSION`.
    ///
    /// Port of `DeflateWithPresetDictCompressionMode`: the chunk is split the same
    /// way as `best_speed`, but the blocks are Deflate streams, which compress much
    /// better and decode much slower.
    [[nodiscard]] static CompressionMode high_compression() {
        return CompressionMode(CompressionModeType::HighCompression);
    }

    [[nodiscard]] CompressionModeType type() const noexcept { return type_; }

    /// `"FAST"`, `"FAST_DECOMPRESSION"`, `"BEST_SPEED"` or `"BEST_COMPRESSION"`.
    [[nodiscard]] std::string_view name() const noexcept;

    [[nodiscard]] std::unique_ptr<Compressor> new_compressor() const;
    [[nodiscard]] std::unique_ptr<Decompressor> new_decompressor() const;

    friend bool operator==(const CompressionMode &lhs, const CompressionMode &rhs) noexcept {
        return lhs.type_ == rhs.type_;
    }

private:
    CompressionModeType type_{CompressionModeType::Fast};
};

} // namespace lucene
