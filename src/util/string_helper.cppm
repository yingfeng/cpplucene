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

export module lucene_core:string_helper;

import :types;
import std;

import :error;

namespace lucene {

/// Methods for manipulating strings.  Port of `core/util/string_helper.rs`.
///
/// The rlucene signatures are generic over [`BytesRef`]; here the borrowed form
/// is `std::span<const uint8_t>` so the helpers stay allocation free.
export struct StringHelper {
    /// A deterministic-per-run seed used for hashing, which prevents hash key
    /// collision attacks and keeps test failures reproducible.
    [[nodiscard]] static int32_t good_fast_hash_seed() noexcept;

    /// Compares two byte ranges element by element and returns the number of
    /// common elements from the start.  Assumes `current_term` comes after
    /// `prior_term`; returns `illegal_argument` otherwise.
    [[nodiscard]] static Result<size_t> bytes_difference(std::span<const uint8_t> prior_term,
                                                         std::span<const uint8_t> current_term);

    /// Length of `current_term` needed as a sort key so that comparison still
    /// returns the same result.  Assumes `current_term` comes after `prior_term`.
    [[nodiscard]] static Result<size_t> sort_key_length(std::span<const uint8_t> prior_term,
                                                        std::span<const uint8_t> current_term);

    /// `true` when `ref` starts with `prefix`.
    [[nodiscard]] static bool starts_with(std::span<const uint8_t> ref, std::span<const uint8_t> prefix) noexcept;

    /// `true` when `ref` starts with `prefix` given as explicit offset/length.
    [[nodiscard]] static bool starts_with(std::span<const uint8_t> ref,
                                          size_t ref_offset,
                                          size_t ref_length,
                                          std::span<const uint8_t> prefix,
                                          size_t prefix_offset,
                                          size_t prefix_length) noexcept;

    /// `true` when `ref` ends with `suffix`.
    [[nodiscard]] static bool ends_with(std::span<const uint8_t> ref, std::span<const uint8_t> suffix) noexcept;

    /// MurmurHash3_x86_32 over `data[offset, offset + len)`.
    ///
    /// Original source/tests at <https://github.com/yonik/java_util>.
    [[nodiscard]] static int32_t murmurhash3_x86_32(std::span<const uint8_t> data, size_t offset, size_t len, int32_t seed);

    /// MurmurHash3_x86_32 over the whole span.
    [[nodiscard]] static int32_t murmurhash3_x86_32(std::span<const uint8_t> data, int32_t seed) {
        return murmurhash3_x86_32(data, 0, data.size(), seed);
    }

    // ---- UTF-8 ----

    /// Decodes the code point starting at `pos` of `utf8`; `num_bytes` receives
    /// how many bytes it occupies.  Mirrors `UnicodeUtil::code_point_at` of
    /// rlucene and fails on malformed or truncated input.
    [[nodiscard]] static Result<int32_t> utf8_code_point_at(std::span<const uint8_t> utf8, size_t pos, size_t &num_bytes);
    /// [`utf8_code_point_at`] over UTF-8 text.
    [[nodiscard]] static Result<int32_t> utf8_code_point_at(std::string_view text, size_t pos, size_t &num_bytes);

    /// Appends `code_point` to `out`, encoded as UTF-8; fails when it is not a
    /// Unicode scalar value.
    [[nodiscard]] static VoidResult utf8_append(std::string &out, int32_t code_point);

    /// Decodes all of `text` into code points; fails on malformed input.
    [[nodiscard]] static Result<std::vector<int32_t>> utf8_to_code_points(std::string_view text);

    /// Length of a Lucene ID, in bytes.
    static constexpr size_t ID_LENGTH = 16;

    /// A unique 16-byte id, big-endian counter starting from a scattered seed.
    [[nodiscard]] static std::array<uint8_t, ID_LENGTH> random_id();

    /// Base-36 rendering of an id, or the literal `"(null)"` for an empty id.
    [[nodiscard]] static std::string id_to_string(std::span<const uint8_t> id);
};

} // namespace lucene
