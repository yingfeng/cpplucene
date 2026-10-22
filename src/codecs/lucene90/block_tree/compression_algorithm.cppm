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

export module lucene_core:compression_algorithm;

import :types;
import std;

import :data_input;
import :error;

namespace lucene {

/// Compression algorithm used for the suffixes of a block of terms.
///
/// Port of `CompressionAlgorithm` (`core/codecs/lucene90/block_tree/compression_algorithm.rs`).
///
/// **Deviation:** the reference implementation attaches `code`, `by_code` and `read` as methods;
/// scoped enums cannot have members in C++, so they are free functions below.
export enum class CompressionAlgorithm : uint8_t {
    NoCompression = 0x00,
    LowercaseAscii = 0x01,
    Lz4 = 0x02,
};

/// The on-disk code of `algorithm`.
[[nodiscard]] constexpr uint8_t compression_algorithm_code(CompressionAlgorithm algorithm) noexcept {
    return static_cast<uint8_t>(algorithm);
}

/// The algorithm for the on-disk `code`.
[[nodiscard]] Result<CompressionAlgorithm> compression_algorithm_by_code(uint8_t code);

/// The `Debug` name the reference implementation prints in the block tree statistics.
[[nodiscard]] std::string_view compression_algorithm_name(CompressionAlgorithm algorithm) noexcept;

/// Reads `out[0..len)`, which was written with `algorithm`.
[[nodiscard]] VoidResult read_compressed(CompressionAlgorithm algorithm, DataInput &input, std::span<uint8_t> out,
                                         int32_t len);

} // namespace lucene
