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

export module lucene_core:longs_ref;

import :types;
import std;

import :error;

namespace lucene {

/// A view over a range of `int64_t` values: `(longs, offset, length)`.
///
/// Port of `util/longs_ref.rs`, following the same shape as [`IntsRef`]: the
/// container is always an owned `std::vector<int64_t>`, and borrowed views are
/// spelled `std::span<const int64_t>` at API boundaries.
export class LongsRef {
public:
    LongsRef() = default;
    explicit LongsRef(std::vector<int64_t> longs);

    /// A view over `[offset, offset + length)` of `longs`.
    [[nodiscard]] static LongsRef from_slice(std::vector<int64_t> longs, size_t offset, size_t length);

    /// Copies the active range into a freshly allocated `LongsRef`.
    [[nodiscard]] static LongsRef deep_copy_of(const LongsRef &other);

    [[nodiscard]] std::vector<int64_t> &longs() noexcept { return longs_; }
    [[nodiscard]] const std::vector<int64_t> &longs() const noexcept { return longs_; }
    [[nodiscard]] size_t offset() const noexcept { return offset_; }
    [[nodiscard]] size_t length() const noexcept { return length_; }
    void set_offset(size_t offset) noexcept { offset_ = offset; }
    void set_length(size_t length) noexcept { length_ = length; }

    /// The active longs.
    [[nodiscard]] std::span<const int64_t> as_slice() const;
    [[nodiscard]] std::span<int64_t> as_mutable_slice();

    /// Lexicographic comparison.
    [[nodiscard]] int32_t compare_to(const LongsRef &other) const;
    [[nodiscard]] bool equals(const LongsRef &other) const { return compare_to(other) == 0; }

    /// Checks that `[offset, offset + length)` fits into `longs`.
    [[nodiscard]] VoidResult is_valid() const;

    [[nodiscard]] std::string to_string() const;

private:
    std::vector<int64_t> longs_{};
    size_t offset_{0};
    size_t length_{0};
};

/// Orders [`LongsRef`]s lexicographically.
///
/// Port of `LongsRefComparator` (`util/longs_ref.rs`).  Lexicographic order is
/// not the natural order of the signed values: like the reference, it compares
/// element by element and then by length, which is what the FST outputs need.
export struct LongsRefComparator {
    [[nodiscard]] static int32_t compare(std::span<const int64_t> left, std::span<const int64_t> right);
    [[nodiscard]] static int32_t compare(const LongsRef &left, const LongsRef &right) {
        return compare(left.as_slice(), right.as_slice());
    }
};

} // namespace lucene
