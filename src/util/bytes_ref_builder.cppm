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

export module clucene:bytes_ref_builder;

import :types;
import std;

import :error;
import :bytes_ref;

namespace clucene {

/// A builder for [`BytesRef`] instances.  Port of `core/index/bytes_ref_builder.rs`.
export class BytesRefBuilder {
public:
    BytesRefBuilder() = default;

    /// A read-only view of the bytes held by this builder.
    [[nodiscard]] const BytesRef &bytes() const noexcept { return bytes_ref_; }
    /// Mutable access to the underlying `BytesRef`.
    [[nodiscard]] BytesRef &bytes_mut() noexcept { return bytes_ref_; }

    [[nodiscard]] size_t length() const noexcept { return bytes_ref_.length(); }
    void set_length(size_t length) noexcept { bytes_ref_.set_length(length); }

    [[nodiscard]] uint8_t byte_at(size_t index) const;
    void set_byte_at(size_t index, uint8_t value);

    /// Grows the internal storage to at least `capacity` bytes, keeping content.
    [[nodiscard]] VoidResult grow(size_t capacity);
    /// Grows to at least `capacity` bytes; existing content may be discarded.
    [[nodiscard]] VoidResult grow_no_copy(size_t capacity);

    [[nodiscard]] VoidResult append_byte(uint8_t b);
    [[nodiscard]] VoidResult append(std::span<const uint8_t> bytes);
    [[nodiscard]] VoidResult append(const BytesRef &ref) { return append(ref.as_slice()); }
    [[nodiscard]] VoidResult append(const BytesRefBuilder &other) { return append(other.bytes_ref_.as_slice()); }

    void clear() noexcept { bytes_ref_.set_length(0); }

    /// Replaces the content with `bytes[off, off + len)`.
    [[nodiscard]] VoidResult copy_bytes(std::span<const uint8_t> bytes, size_t off, size_t len);
    [[nodiscard]] VoidResult copy_bytes(const BytesRef &ref) { return copy_bytes(ref.bytes(), ref.offset(), ref.length()); }
    [[nodiscard]] VoidResult copy_bytes(const BytesRefBuilder &other) {
        return copy_bytes(other.bytes_ref_.bytes(), other.bytes_ref_.offset(), other.bytes_ref_.length());
    }
    /// Replaces the content with the UTF-8 encoding of `text`.
    [[nodiscard]] VoidResult copy_chars(std::string_view text);
    /// Replaces the content with the UTF-8 encoding of `chars[off, off + len)`.
    [[nodiscard]] VoidResult copy_chars(std::span<const char32_t> chars, size_t off, size_t len);

    /// The internal `BytesRef`; its offset must stay 0.
    [[nodiscard]] BytesRef &get_bytes_mut_ref() noexcept { return bytes_ref_; }
    [[nodiscard]] const BytesRef &get_bytes_ref() const noexcept { return bytes_ref_; }
    /// Moves the content out, leaving the builder empty.
    [[nodiscard]] BytesRef get_bytes_owner();
    /// A deep copy of the current content.
    [[nodiscard]] Result<BytesRef> get_bytes_ref_copy() const;

private:
    BytesRef bytes_ref_{};
};

} // namespace clucene
