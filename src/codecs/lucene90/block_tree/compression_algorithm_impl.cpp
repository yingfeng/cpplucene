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

module lucene_core:compression_algorithm.impl;

import :compression_algorithm;

import :data_input;
import :debug;
import :error;
import :lowercase_ascii_compression;
import :lz4;
import :types;

import std;

namespace lucene {

Result<CompressionAlgorithm> compression_algorithm_by_code(uint8_t code) {
    switch (code) {
        case 0x00:
            return CompressionAlgorithm::NoCompression;
        case 0x01:
            return CompressionAlgorithm::LowercaseAscii;
        case 0x02:
            return CompressionAlgorithm::Lz4;
        default:
            return err(LuceneError::illegal_argument(
                    std::format("Illegal code for a compression algorithm: {}", code)));
    }
}

std::string_view compression_algorithm_name(CompressionAlgorithm algorithm) noexcept {
    switch (algorithm) {
        case CompressionAlgorithm::NoCompression:
            return "NoCompression";
        case CompressionAlgorithm::LowercaseAscii:
            return "LowercaseAscii";
        case CompressionAlgorithm::Lz4:
            return "Lz4";
    }
    return "Unknown";
}

VoidResult read_compressed(CompressionAlgorithm algorithm, DataInput &input, std::span<uint8_t> out, int32_t len) {
    switch (algorithm) {
        case CompressionAlgorithm::NoCompression: {
            if (len < 0) {
                return err(LuceneError::illegal_argument(std::format("negative length: {}", len)));
            }
            const size_t count = static_cast<size_t>(len);
            return input.read_bytes(out.subspan(0, count));
        }
        case CompressionAlgorithm::LowercaseAscii: {
            debug_check(len >= 0);
            if (len < 0) {
                return err(LuceneError::illegal_argument(std::format("negative length: {}", len)));
            }
            return LowercaseAsciiCompression::decompress(input, out, static_cast<size_t>(len));
        }
        case CompressionAlgorithm::Lz4: {
            auto decompressed = LZ4::decompress(input, len, out, 0);
            if (!decompressed) {
                return std::unexpected(std::move(decompressed.error()));
            }
            return {};
        }
    }
    return err(LuceneError::illegal_argument("unknown compression algorithm"));
}

} // namespace lucene
