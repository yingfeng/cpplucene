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

export module lucene_core:bytes_ref;

import :types;
import :debug;
import std;

import :error;

namespace lucene {

/// A slice (`offset` + `length`) into an owned byte array.
///
/// Port of `core/index/bytes_ref.rs` (`BytesRef<Vec<u8>>`).
///
/// The reference implementation makes `BytesRef` generic over its byte container so it can model both
/// owned (`Vec<u8>`) and borrowed (`&[u8]`) references.  In C++ the borrowed
/// form is spelled `std::span<const uint8_t>` at API boundaries, so `BytesRef`
/// here always owns its storage; functions that accept either take a span.
///
/// Sorting: byte arrays are compared lexicographically with unsigned bytes,
/// which matches the Rust `Ord` implementation.
export class BytesRef {
public:
    BytesRef() = default;

    /// `BytesRef::from_bytes`
    explicit BytesRef(std::vector<uint8_t> bytes);

    /// `BytesRef::from_string`: initializes the byte container from UTF-8 text.
    [[nodiscard]] static BytesRef from_string(std::string_view text);

    /// `BytesRef::from_slice`
    [[nodiscard]] static BytesRef from_slice(std::vector<uint8_t> bytes, size_t offset, size_t length);

    /// Copies the active range of `other` into a freshly allocated `BytesRef`.
    [[nodiscard]] static Result<BytesRef> deep_copy_of(const BytesRef &other);

    // ---- storage ----
    [[nodiscard]] std::vector<uint8_t> &bytes() noexcept { return bytes_; }
    [[nodiscard]] const std::vector<uint8_t> &bytes() const noexcept { return bytes_; }

    [[nodiscard]] size_t offset() const noexcept { return offset_; }
    [[nodiscard]] size_t length() const noexcept { return length_; }

    void set_offset(size_t offset) noexcept { offset_ = offset; }
    void set_length(size_t length) noexcept { length_ = length; }

    /// The active byte range.
    [[nodiscard]] std::span<const uint8_t> as_slice() const {
        debug_check(offset_ + length_ <= bytes_.size());
        return std::span<const uint8_t>(bytes_.data() + offset_, length_);
    }

    /// The active byte range, mutable.
    [[nodiscard]] std::span<uint8_t> as_mutable_slice() {
        debug_check(offset_ + length_ <= bytes_.size());
        return std::span<uint8_t>(bytes_.data() + offset_, length_);
    }

    // ---- content operations ----
    /// Replaces the contents with a copy of `bytes`, reusing the existing
    /// allocation when its capacity is sufficient. Resets the offset to zero.
    void copy_from_slice(std::span<const uint8_t> bytes);

    /// Compares this value with another byte range.
    [[nodiscard]] int32_t compare_to(std::span<const uint8_t> other) const;

    /// Compares this value with another `BytesRef`.
    [[nodiscard]] int32_t compare_to(const BytesRef &other) const { return compare_to(other.as_slice()); }

    /// `true` when the active ranges are byte-identical.
    [[nodiscard]] bool bytes_equals(std::span<const uint8_t> other) const { return std::ranges::equal(as_slice(), other); }

    /// Internal consistency checks on the active byte range.
    [[nodiscard]] VoidResult is_valid() const;

    /// Interprets the active bytes as UTF-8.
    [[nodiscard]] Result<std::string> utf8_to_string() const;

    /// Moves the byte container out of this instance.
    [[nodiscard]] std::vector<uint8_t> take_bytes();

    /// MurmurHash3_x86_32 of the active range, using `GOOD_FAST_HASH_SEED`.
    [[nodiscard]] int32_t hash_code() const;

    /// Debug rendering identical to the reference implementation's `Display`, e.g. `[61 62 63]`.
    [[nodiscard]] std::string to_display_string() const;

    [[nodiscard]] bool operator==(const BytesRef &other) const { return std::ranges::equal(as_slice(), other.as_slice()); }
    [[nodiscard]] std::strong_ordering operator<=>(const BytesRef &other) const;

private:
    std::vector<uint8_t> bytes_{};
    size_t offset_{0};
    size_t length_{0};
};

} // namespace lucene

export namespace lucene {

inline std::ostream &operator<<(std::ostream &os, const BytesRef &ref) { return os << ref.to_display_string(); }

} // namespace lucene
