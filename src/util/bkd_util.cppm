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

export module lucene_core:bkd_util;

import :types;

import std;

namespace lucene {

/// Byte level helpers shared by the BKD writer, reader and sorter.
///
/// Port of `BKDUtil` (`util/bkd/bkd_util.rs`).
export struct BKDUtil {
    /// Length of the common prefix of the next 8 bytes of `a`/`b` (`0..8`).
    [[nodiscard]] static int32_t common_prefix_length8(std::span<const uint8_t> a, size_t a_offset,
                                                       std::span<const uint8_t> b, size_t b_offset);

    /// Length of the common prefix of the next 4 bytes of `a`/`b` (`0..4`).
    [[nodiscard]] static int32_t common_prefix_length4(std::span<const uint8_t> a, size_t a_offset,
                                                       std::span<const uint8_t> b, size_t b_offset);

    /// Length of the common prefix of the next `num_bytes` bytes of `a`/`b`.
    [[nodiscard]] static int32_t common_prefix_length_n(std::span<const uint8_t> a, size_t a_offset,
                                                        std::span<const uint8_t> b, size_t b_offset, size_t num_bytes);

    /// Whether the next 8 bytes are identical.
    [[nodiscard]] static bool equals8(std::span<const uint8_t> a, size_t a_offset, std::span<const uint8_t> b,
                                      size_t b_offset);

    /// Whether the next 4 bytes are identical.
    [[nodiscard]] static bool equals4(std::span<const uint8_t> a, size_t a_offset, std::span<const uint8_t> b,
                                      size_t b_offset);

    /// Unsigned lexicographic comparison of the next `num_bytes` bytes.
    ///
    /// Replaces rlucene's `ArrayUtil::get_unsigned_comparator`, whose four
    /// implementations (`int32`, `int64`, generic) collapse into one loop here.
    [[nodiscard]] static int32_t compare_unsigned(std::span<const uint8_t> a, size_t a_offset,
                                                  std::span<const uint8_t> b, size_t b_offset, size_t num_bytes);
};

/// "Do the next `num_bytes` bytes match?" for a fixed width.
///
/// Replaces rlucene's `ByteArrayPredicate` trait plus its three
/// implementations: with a stored width the dispatch collapses into one
/// comparison, and the 4/8 byte cases keep their integer fast paths.
export struct ByteArrayPredicate {
    size_t num_bytes{0};

    [[nodiscard]] bool test(std::span<const uint8_t> a, size_t a_offset, std::span<const uint8_t> b,
                            size_t b_offset) const;
};

/// "How long is the common prefix of the next `num_bytes` bytes?"
export struct ByteArrayComparator {
    size_t num_bytes{0};

    [[nodiscard]] int32_t common_prefix_length(std::span<const uint8_t> a, size_t a_offset,
                                               std::span<const uint8_t> b, size_t b_offset) const;
};

} // namespace lucene
