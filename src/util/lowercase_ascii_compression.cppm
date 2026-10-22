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

export module lucene_core:lowercase_ascii_compression;

import :types;
import :error;

import std;

import :data_input;
import :data_output;

namespace lucene {

/// Compresses arrays that mostly contain characters in `[0x1F, 0x3F)` or
/// `[0x5F, 0x7F)`, which notably include all digits, lowercase letters, `.`,
/// `-` and `_`.
///
/// Port of `util/compress/lowercase_ascii_compression.rs` /
/// `compression/LowercaseAsciiCompression`.  Compressible bytes are packed into
/// 6 bits and the 2 leading bits are stored in a separate bitmap; the
/// exceptions (at most one in 32 bytes) are appended as `(delta, value)` pairs.
export class LowercaseAsciiCompression {
public:
    /// Compresses `input[0..len]` into `out`, returning `false` when the content
    /// is not compressible.  `tmp` must hold at least `len` bytes and is used as
    /// scratch space.  On success fewer than `len` bytes are written.
    [[nodiscard]] static Result<bool> compress(std::span<const uint8_t> input, size_t len, std::span<uint8_t> tmp,
                                               DataOutput &out);

    /// Decompresses into `out[0..len]`; `len` is the *uncompressed* length.
    [[nodiscard]] static VoidResult decompress(DataInput &input, std::span<uint8_t> out, size_t len);
};

} // namespace lucene
