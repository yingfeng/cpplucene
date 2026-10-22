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

export module lucene_core:ints_ref;

import :types;
import :error;

import std;

namespace lucene {

/// A view over a range of `int32_t` values: `(ints, offset, length)`.
///
/// Port of `util/ints_ref.rs`.  Unlike the reference implementation, which parametrizes the container
/// (`Vec<i32>` or `Arc<Vec<i32>>`), the C++ version always owns a
/// `std::vector<int32_t>`; borrowed views are spelled `std::span<const int32_t>`
/// at API boundaries.
export class IntsRef {
public:
    IntsRef() = default;
    explicit IntsRef(std::vector<int32_t> ints);

    /// A view over `[offset, offset + length)` of `ints`.
    [[nodiscard]] static IntsRef from_slice(std::vector<int32_t> ints, size_t offset, size_t length);

    /// Copies the active range into a freshly allocated `IntsRef`.
    [[nodiscard]] static IntsRef deep_copy_of(const IntsRef &other);

    [[nodiscard]] std::vector<int32_t> &ints() noexcept { return ints_; }
    [[nodiscard]] const std::vector<int32_t> &ints() const noexcept { return ints_; }
    [[nodiscard]] size_t offset() const noexcept { return offset_; }
    [[nodiscard]] size_t length() const noexcept { return length_; }
    void set_offset(size_t offset) noexcept { offset_ = offset; }
    void set_length(size_t length) noexcept { length_ = length; }

    /// The active ints.
    [[nodiscard]] std::span<const int32_t> as_slice() const;
    [[nodiscard]] std::span<int32_t> as_mutable_slice();

    /// Lexicographic comparison, used to keep FST inputs sorted.
    [[nodiscard]] int32_t compare_to(const IntsRef &other) const;
    [[nodiscard]] bool equals(const IntsRef &other) const { return compare_to(other) == 0; }

    /// Checks that `[offset, offset + length)` fits into `ints`.
    [[nodiscard]] VoidResult is_valid() const;

    [[nodiscard]] std::string to_string() const;

private:
    std::vector<int32_t> ints_{};
    size_t offset_{0};
    size_t length_{0};
};

/// Builds an [`IntsRef`] incrementally while keeping its backing array reusable.
///
/// Port of `util/ints_ref_builder.rs`.
export class IntsRefBuilder {
public:
    IntsRefBuilder() = default;

    [[nodiscard]] std::vector<int32_t> &ints() noexcept { return ints_; }
    [[nodiscard]] const std::vector<int32_t> &ints() const noexcept { return ints_; }
    [[nodiscard]] size_t length() const noexcept { return length_; }
    void set_length(size_t length) noexcept { length_ = length; }
    void clear() noexcept { length_ = 0; }

    [[nodiscard]] int32_t int_at(size_t offset) const;
    void set_int_at(size_t offset, int32_t value);

    /// Appends one value.
    [[nodiscard]] VoidResult append(int32_t value);
    /// Grows the used length, keeping the existing values.
    [[nodiscard]] VoidResult grow(size_t new_length);
    /// Grows the used length without preserving the values.
    [[nodiscard]] VoidResult grow_no_copy(size_t new_length);
    [[nodiscard]] VoidResult copy_ints(std::span<const int32_t> other, size_t other_offset, size_t other_length);
    [[nodiscard]] VoidResult copy_ints_ref(const IntsRef &other);

    /// The built value (a copy of the used prefix).
    [[nodiscard]] IntsRef to_ints_ref() const;

private:
    std::vector<int32_t> ints_{};
    size_t length_{0};
};

} // namespace lucene
