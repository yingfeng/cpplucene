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

module lucene_core:compression.impl;

import :compression;

import :data_input;
import :data_output;
import :deflate_with_preset_dict_compression_mode;
import :error;
import :lz4;
import :types;

import std;

namespace lucene {

namespace {

/// Extra padding so LZ4 may copy eight bytes at a time past the end.
constexpr size_t DECOMPRESSOR_PADDING = 7;

/// LZ4 with the greedy hash table.
class Lz4FastCompressor final : public Compressor {
public:
    [[nodiscard]] VoidResult compress(std::span<const uint8_t> data, DataOutput &out) override {
        return LZ4::compress(data, 0, static_cast<int32_t>(data.size()), out, table_);
    }

    [[nodiscard]] VoidResult close() override { return {}; }

private:
    FastCompressionHashTable table_;
};

/// LZ4 with the chain-table hash table: slower to write, same decompressor.
class Lz4HighCompressor final : public Compressor {
public:
    [[nodiscard]] VoidResult compress(std::span<const uint8_t> data, DataOutput &out) override {
        return LZ4::compress(data, 0, static_cast<int32_t>(data.size()), out, table_);
    }

    [[nodiscard]] VoidResult close() override { return {}; }

private:
    HighCompressionHashTable table_;
};

/// Port of `LZ4Decompressor`.
class Lz4Decompressor final : public Decompressor {
public:
    [[nodiscard]] VoidResult decompress(DataInput &compressed, int32_t original_length, int32_t offset,
                                        int32_t length, std::vector<uint8_t> &dest) override {
        if (offset < 0 || length < 0 || offset + length > original_length) {
            return err(LuceneError::illegal_argument("the requested range is outside the compressed chunk"));
        }
        const auto padded_length = static_cast<size_t>(original_length) + DECOMPRESSOR_PADDING;
        if (dest.size() < padded_length) {
            dest.resize(padded_length, 0);
        }
        // LZ4 has no random access: everything before `offset + length` is
        // decoded, and the caller reads the tail of the buffer.
        auto decompressed = LZ4::decompress(compressed, offset + length, std::span<uint8_t>(dest), 0);
        if (!decompressed.has_value()) {
            return std::unexpected(std::move(decompressed.error()));
        }
        if (decompressed.value() > original_length) {
            return err(LuceneError::corrupt_index("corrupted: decompressed " +
                                                 std::to_string(decompressed.value()) +
                                                 " bytes, expected at most " +
                                                 std::to_string(original_length)));
        }
        return {};
    }

    [[nodiscard]] std::unique_ptr<Decompressor> clone() const override {
        return std::unique_ptr<Decompressor>(new Lz4Decompressor());
    }
};


/// A `DataOutput` that appends into a `std::vector`, used to measure the size
/// of a compressed block before its length is written.
class VectorDataOutput final : public DataOutput {
public:
    void reset() noexcept { bytes.clear(); }

    [[nodiscard]] VoidResult write_byte(uint8_t b) override {
        bytes.push_back(b);
        return {};
    }

    [[nodiscard]] VoidResult write_bytes(std::span<const uint8_t> b, size_t offset, size_t length) override {
        if (offset > b.size() || length > b.size() - offset) {
            return err(LuceneError::array_index_out_of_bounds("the write range is outside the buffer"));
        }
        bytes.insert(bytes.end(), b.begin() + static_cast<std::ptrdiff_t>(offset),
                     b.begin() + static_cast<std::ptrdiff_t>(offset + length));
        return {};
    }

    std::vector<uint8_t> bytes;
};

/// Port of `LZ4WithPresetDictCompressor`: ten sub blocks compressed against a
/// dictionary block.
class Lz4PresetDictCompressor final : public Compressor {
public:
    /// Shoot for ten sub blocks.
    static constexpr int32_t NUM_SUB_BLOCKS = 10;
    /// And a dictionary about twice as small as a sub block.
    static constexpr int32_t DICT_SIZE_FACTOR = 2;

    [[nodiscard]] VoidResult compress(std::span<const uint8_t> data, DataOutput &out) override {
        const auto len = static_cast<int32_t>(data.size());
        const int32_t dict_length = std::min(len / (NUM_SUB_BLOCKS * DICT_SIZE_FACTOR), LZ4::MAX_DISTANCE);
        const int32_t block_length = (len - dict_length + NUM_SUB_BLOCKS - 1) / NUM_SUB_BLOCKS;
        buffer_.assign(static_cast<size_t>(dict_length + block_length), 0);

        if (auto written = out.write_vint(dict_length); !written) {
            return written;
        }
        if (auto written = out.write_vint(block_length); !written) {
            return written;
        }

        compressed_.reset();
        // The dictionary is compressed first, without a dictionary of its own.
        std::copy_n(data.begin(), dict_length, buffer_.begin());
        if (auto compressed = compress_block(0, dict_length, out); !compressed) {
            return compressed;
        }

        int32_t start = dict_length;
        while (start < len) {
            const int32_t block = std::min(block_length, len - start);
            std::copy_n(data.begin() + start, block, buffer_.begin() + dict_length);
            if (auto compressed = compress_block(dict_length, block, out); !compressed) {
                return compressed;
            }
            start += block_length;
        }
        // Only the lengths were written so far; the payload follows them.
        return out.write_bytes(compressed_.bytes, 0, compressed_.bytes.size());
    }

    [[nodiscard]] VoidResult close() override { return {}; }

private:
    /// Compresses `buffer_[dict_len, dict_len + len)` into `compressed_` and
    /// writes the compressed length to `out`.
    [[nodiscard]] VoidResult compress_block(int32_t dict_len, int32_t len, DataOutput &out) {
        const size_t before = compressed_.bytes.size();
        if (auto compressed = LZ4::compress_with_dictionary(buffer_, 0, dict_len, len, compressed_, table_);
            !compressed) {
            return compressed;
        }
        return out.write_vint(static_cast<int32_t>(compressed_.bytes.size() - before));
    }

    VectorDataOutput compressed_;
    FastCompressionHashTable table_;
    std::vector<uint8_t> buffer_;
};

/// Port of `LZ4WithPresetDictDecompressor`.
class Lz4PresetDictDecompressor final : public Decompressor {
public:
    [[nodiscard]] VoidResult decompress(DataInput &compressed, int32_t original_length, int32_t offset,
                                        int32_t length, std::vector<uint8_t> &dest) override {
        if (offset < 0 || length < 0 || offset + length > original_length) {
            return err(LuceneError::illegal_argument("the requested range is outside the compressed chunk"));
        }
        if (length == 0) {
            return {};
        }
        auto dict_length = compressed.read_vint();
        if (!dict_length) {
            return std::unexpected(std::move(dict_length.error()));
        }
        auto block_length = compressed.read_vint();
        if (!block_length) {
            return std::unexpected(std::move(block_length.error()));
        }
        if (dict_length.value() < 0 || block_length.value() <= 0) {
            return err(LuceneError::corrupt_index("the preset dictionary header is invalid"));
        }
        if (auto lengths = read_compressed_lengths(compressed, original_length, dict_length.value(),
                                                   block_length.value());
            !lengths) {
            return lengths;
        }

        const auto dilated = static_cast<size_t>(original_length) + DECOMPRESSOR_PADDING;
        if (dest.size() < dilated) {
            dest.resize(dilated, 0);
        }
        buffer_.assign(static_cast<size_t>(dict_length.value() + block_length.value()), 0);

        // The dictionary is decoded first and stays at the front of `buffer_` so
        // that every sub block can use it as its window.
        auto decoded = LZ4::decompress(compressed, dict_length.value(), std::span<uint8_t>(buffer_), 0);
        if (!decoded.has_value()) {
            return std::unexpected(std::move(decoded.error()));
        }
        if (decoded.value() != dict_length.value()) {
            return err(LuceneError::corrupt_index("the preset dictionary has an unexpected length"));
        }
        std::copy_n(buffer_.begin(), dict_length.value(), dest.begin());

        // **Deviation:** the reference skips whole sub blocks that end before
        // `offset`; this port decodes the prefix instead, which is at most one
        // chunk (80 KB by default) and keeps the loop to a single form.
        int32_t produced = dict_length.value();
        while (produced < offset + length) {
            const int32_t remaining = original_length - produced;
            if (remaining <= 0) {
                break;
            }
            const int32_t block = std::min(block_length.value(), remaining);
            auto block_decoded =
                    LZ4::decompress(compressed, block, std::span<uint8_t>(buffer_), dict_length.value());
            if (!block_decoded.has_value()) {
                return std::unexpected(std::move(block_decoded.error()));
            }
            std::copy_n(buffer_.begin() + dict_length.value(), block, dest.begin() + produced);
            produced += block_length.value();
        }
        return {};
    }

    [[nodiscard]] std::unique_ptr<Decompressor> clone() const override {
        return std::unique_ptr<Decompressor>(new Lz4PresetDictDecompressor());
    }

private:
    /// Reads the compressed length of the dictionary (unused) and of every sub
    /// block, leaving the input at the dictionary payload.
    [[nodiscard]] VoidResult read_compressed_lengths(DataInput &compressed, int32_t original_length,
                                                     int32_t dict_length, int32_t block_length) {
        auto dict_compressed = compressed.read_vint();
        if (!dict_compressed) {
            return std::unexpected(std::move(dict_compressed.error()));
        }
        int32_t total = dict_length;
        while (total < original_length) {
            auto block = compressed.read_vint();
            if (!block) {
                return std::unexpected(std::move(block.error()));
            }
            total += block_length;
        }
        return {};
    }

    /// The dictionary followed by the block being decoded.
    std::vector<uint8_t> buffer_;
};

} // namespace

std::string_view CompressionMode::name() const noexcept {
    switch (type_) {
        case CompressionModeType::Fast:
            return "FAST";
        case CompressionModeType::FastDecompression:
            return "FAST_DECOMPRESSION";
        case CompressionModeType::BestSpeed:
            return "BEST_SPEED";
        case CompressionModeType::HighCompression:
            return "BEST_COMPRESSION";
    }
    return "FAST";
}

std::unique_ptr<Compressor> CompressionMode::new_compressor() const {
    switch (type_) {
        case CompressionModeType::FastDecompression:
            return std::unique_ptr<Compressor>(new Lz4HighCompressor());
        case CompressionModeType::BestSpeed:
            return std::unique_ptr<Compressor>(new Lz4PresetDictCompressor());
        case CompressionModeType::HighCompression:
            return std::unique_ptr<Compressor>(new DeflateWithPresetDictCompressor());
        case CompressionModeType::Fast:
            break;
    }
    return std::unique_ptr<Compressor>(new Lz4FastCompressor());
}

std::unique_ptr<Decompressor> CompressionMode::new_decompressor() const {
    if (type_ == CompressionModeType::BestSpeed) {
        return std::unique_ptr<Decompressor>(new Lz4PresetDictDecompressor());
    }
    if (type_ == CompressionModeType::HighCompression) {
        return std::unique_ptr<Decompressor>(new DeflateWithPresetDictDecompressor());
    }
    return std::unique_ptr<Decompressor>(new Lz4Decompressor());
}

} // namespace lucene
