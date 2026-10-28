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

// zlib is a C library, so its header has to be included before the module
// declaration: the classes below are defined inline in this unit, which keeps the
// header out of every other translation unit.
#include <zlib.h>

export module lucene_core:deflate_with_preset_dict_compression_mode;

import :types;
import std;

import :compression;
import :data_input;
import :data_output;
import :error;

namespace lucene {

namespace {

/// How many sub blocks a chunk is split into, and how much smaller the dictionary
/// is than a sub block.
constexpr int32_t kNumSubBlocks = 10;
constexpr int32_t kDictSizeFactor = 6;

/// A raw deflate stream: no zlib header, no trailing checksum, which is what the
/// format writes.
constexpr int32_t kWindowBits = -15;
/// The level the reference implementation uses: the highest one that does not
/// evaluate matches lazily.
constexpr int32_t kCompressionLevel = 6;

/// Compresses `input` into a complete raw deflate stream, optionally against a
/// preset dictionary.
[[nodiscard]] VoidResult deflate_block(std::span<const uint8_t> input, std::span<const uint8_t> dictionary,
                                       std::vector<uint8_t> &out) {
    z_stream stream{};
    if (deflateInit2(&stream, kCompressionLevel, Z_DEFLATED, kWindowBits, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return err(LuceneError::illegal_state("zlib refused to start a deflate stream"));
    }
    if (!dictionary.empty() &&
        deflateSetDictionary(&stream, dictionary.data(), static_cast<uInt>(dictionary.size())) != Z_OK) {
        deflateEnd(&stream);
        return err(LuceneError::illegal_state("zlib refused the preset dictionary"));
    }

    out.resize(std::max<size_t>(input.size() + 64, 64));
    stream.next_in = const_cast<Bytef *>(input.data());
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());

    int status = deflate(&stream, Z_FINISH);
    while (status == Z_OK && stream.avail_out == 0) {
        // The buffer filled up before the stream ended: grow it and carry on.
        const size_t used = out.size();
        out.resize(used * 2);
        stream.next_out = out.data() + used;
        stream.avail_out = static_cast<uInt>(out.size() - used);
        status = deflate(&stream, Z_FINISH);
    }
    const size_t written = stream.total_out;
    deflateEnd(&stream);
    if (status != Z_STREAM_END) {
        return err(LuceneError::illegal_state("the deflate stream did not end as expected"));
    }
    out.resize(written);
    return {};
}

/// Appends the raw deflate stream `input` to `out`, optionally against a preset
/// dictionary.
[[nodiscard]] VoidResult inflate_block(std::span<const uint8_t> input, std::span<const uint8_t> dictionary,
                                       std::vector<uint8_t> &out) {
    z_stream stream{};
    if (inflateInit2(&stream, kWindowBits) != Z_OK) {
        return err(LuceneError::illegal_state("zlib refused to start an inflate stream"));
    }
    if (!dictionary.empty() &&
        inflateSetDictionary(&stream, dictionary.data(), static_cast<uInt>(dictionary.size())) != Z_OK) {
        inflateEnd(&stream);
        return err(LuceneError::illegal_state("zlib refused the preset dictionary"));
    }

    const size_t produced_before = out.size();
    stream.next_in = const_cast<Bytef *>(input.data());
    stream.avail_in = static_cast<uInt>(input.size());

    // A raw stream may need one extra byte the compressor never wrote; the
    // reference implementation feeds a zero byte as well.
    uint8_t padding = 0;
    bool padding_used = false;
    int status = Z_OK;
    while (true) {
        const size_t used = out.size() - produced_before;
        // Room for one more chunk of output, on top of what this block already
        // produced and of what the caller had before it.
        out.resize(produced_before + used + 16 * 1024);
        stream.next_out = out.data() + produced_before + used;
        stream.avail_out = static_cast<uInt>(16 * 1024);
        status = inflate(&stream, Z_FINISH);
        const size_t produced = 16 * 1024 - stream.avail_out;
        out.resize(produced_before + used + produced);
        if (status == Z_STREAM_END) {
            break;
        }
        if (status == Z_BUF_ERROR || (status == Z_OK && stream.avail_in == 0)) {
            // A raw stream may need one extra byte the compressor never wrote; the
            // reference implementation feeds a zero byte as well.
            if (padding_used) {
                inflateEnd(&stream);
                return err(LuceneError::corrupt_index("the deflate stream ended too early"));
            }
            stream.next_in = &padding;
            stream.avail_in = 1;
            padding_used = true;
            continue;
        }
        if (status != Z_OK) {
            inflateEnd(&stream);
            return err(LuceneError::corrupt_index("the deflate stream is corrupt"));
        }
        if (stream.avail_out != 0 && stream.avail_in == 0) {
            // Nothing left to read and room to spare, without an end marker.
            inflateEnd(&stream);
            return err(LuceneError::corrupt_index("the deflate stream ended too early"));
        }
    }
    inflateEnd(&stream);
    return {};
}

} // namespace

/// Compresses a chunk with Deflate, against a preset dictionary.
///
/// Port of `DeflateWithPresetDictCompressor`
/// (`core/codecs/lucene90/deflate_with_preset_dict_compression_mode.rs`).
///
/// The chunk is split into a dictionary block and up to ten sub blocks that are
/// compressed against it, which is what the `HIGH_COMPRESSION` formats read.
export class DeflateWithPresetDictCompressor final : public Compressor {
public:
    [[nodiscard]] VoidResult compress(std::span<const uint8_t> data, DataOutput &out) override {
        const auto len = static_cast<int32_t>(data.size());
        const int32_t dict_length = len / (kNumSubBlocks * kDictSizeFactor);
        const int32_t block_length = (len - dict_length + kNumSubBlocks - 1) / kNumSubBlocks;
        if (auto written = out.write_vint(dict_length); !written.has_value()) {
            return written;
        }
        if (auto written = out.write_vint(block_length); !written.has_value()) {
            return written;
        }

        const auto dict_bytes = data.subspan(0, static_cast<size_t>(dict_length));
        if (auto compressed = compress_block(dict_bytes, {}, out); !compressed.has_value()) {
            return compressed;
        }
        int32_t start = dict_length;
        while (start < len) {
            const int32_t block = std::min(block_length, len - start);
            const auto block_bytes = data.subspan(static_cast<size_t>(start), static_cast<size_t>(block));
            if (auto compressed = compress_block(block_bytes, dict_bytes, out); !compressed.has_value()) {
                return compressed;
            }
            start += block_length;
        }
        return {};
    }

    [[nodiscard]] VoidResult close() override { return {}; }

private:
    /// Writes one deflate stream: its length as a vint, then the bytes.
    [[nodiscard]] VoidResult compress_block(std::span<const uint8_t> block, std::span<const uint8_t> dictionary,
                                            DataOutput &out) {
        if (block.empty()) {
            // An empty block is just a zero length, with no payload.
            return out.write_vint(0);
        }
        compressed_.clear();
        if (auto deflated = deflate_block(block, dictionary, compressed_); !deflated.has_value()) {
            return deflated;
        }
        if (auto written = out.write_vint(static_cast<int32_t>(compressed_.size())); !written.has_value()) {
            return written;
        }
        return out.write_bytes(compressed_, 0, compressed_.size());
    }

    std::vector<uint8_t> compressed_;
};

/// Decompresses a chunk written by [`DeflateWithPresetDictCompressor`].
///
/// Port of `DeflateWithPresetDictDecompressor`
/// (`core/codecs/lucene90/deflate_with_preset_dict_compression_mode.rs`).
///
/// **Deviation:** every block is decoded, even the ones before the requested
/// window, because the port keeps the whole chunk in one buffer instead of
/// skipping blocks it does not need.
export class DeflateWithPresetDictDecompressor final : public Decompressor {
public:
    [[nodiscard]] VoidResult decompress(DataInput &compressed, int32_t original_length, int32_t offset, int32_t length,
                                        std::vector<uint8_t> &dest) override {
        if (offset < 0 || length < 0 || offset + length > original_length) {
            return err(LuceneError::illegal_argument("the requested range is outside the compressed chunk"));
        }
        if (length == 0) {
            return {};
        }
        auto dict_length = compressed.read_vint();
        if (!dict_length.has_value()) {
            return std::unexpected(std::move(dict_length.error()));
        }
        auto block_length = compressed.read_vint();
        if (!block_length.has_value()) {
            return std::unexpected(std::move(block_length.error()));
        }
        if (dict_length.value() < 0 || block_length.value() < 0) {
            return err(LuceneError::corrupt_index("the preset dictionary header is invalid"));
        }

        // The dictionary block first: every sub block is decoded against it.
        buffer_.clear();
        if (auto dictionary = decode_block(compressed, {}, buffer_); !dictionary.has_value()) {
            return dictionary;
        }
        if (buffer_.size() != static_cast<size_t>(dict_length.value())) {
            return err(LuceneError::corrupt_index("the preset dictionary has another length than announced"));
        }
        // The dictionary is kept apart: the chunk grows while the blocks are
        // decoded, and a view into it would be invalidated by that growth.
        dictionary_.assign(buffer_.begin(), buffer_.end());

        // Then the sub blocks, which are decoded one after the other so that
        // `buffer_` holds the original chunk.
        while (buffer_.size() < static_cast<size_t>(original_length)) {
            if (auto block = decode_block(compressed, dictionary_, buffer_); !block.has_value()) {
                return block;
            }
        }
        if (buffer_.size() < static_cast<size_t>(offset + length)) {
            return err(LuceneError::corrupt_index("the compressed chunk is shorter than announced"));
        }

        if (dest.size() < static_cast<size_t>(offset + length)) {
            dest.resize(static_cast<size_t>(offset + length), 0);
        }
        std::copy_n(buffer_.begin() + offset, length, dest.begin() + offset);
        return {};
    }

    [[nodiscard]] std::unique_ptr<Decompressor> clone() const override {
        return std::make_unique<DeflateWithPresetDictDecompressor>();
    }

private:
    /// Reads one block (a length, then that many bytes) and appends it to `out`.
    [[nodiscard]] VoidResult decode_block(DataInput &compressed, std::span<const uint8_t> dictionary,
                                          std::vector<uint8_t> &out) {
        auto compressed_length = compressed.read_vint();
        if (!compressed_length.has_value()) {
            return std::unexpected(std::move(compressed_length.error()));
        }
        if (compressed_length.value() < 0) {
            return err(LuceneError::corrupt_index("a compressed block has a negative length"));
        }
        if (compressed_length.value() == 0) {
            return {};
        }
        std::vector<uint8_t> payload(static_cast<size_t>(compressed_length.value()), 0);
        if (auto read = compressed.read_bytes(payload); !read.has_value()) {
            return read;
        }
        return inflate_block(payload, dictionary, out);
    }

    std::vector<uint8_t> buffer_;
    /// The preset dictionary of the chunk, copied out of [`buffer_`].
    std::vector<uint8_t> dictionary_;
};

} // namespace lucene
