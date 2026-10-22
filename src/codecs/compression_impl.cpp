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

} // namespace

std::string_view CompressionMode::name() const noexcept {
    switch (type_) {
        case CompressionModeType::Fast:
            return "FAST";
        case CompressionModeType::FastDecompression:
            return "FAST_DECOMPRESSION";
    }
    return "FAST";
}

std::unique_ptr<Compressor> CompressionMode::new_compressor() const {
    if (type_ == CompressionModeType::FastDecompression) {
        return std::unique_ptr<Compressor>(new Lz4HighCompressor());
    }
    return std::unique_ptr<Compressor>(new Lz4FastCompressor());
}

std::unique_ptr<Decompressor> CompressionMode::new_decompressor() const {
    return std::unique_ptr<Decompressor>(new Lz4Decompressor());
}

} // namespace lucene
