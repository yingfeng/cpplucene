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

export module lucene_core:ram_usage_estimator;

import :types;
import std;

namespace lucene {

/// The RAM a query is assumed to use when it reports nothing, `Query`'s
/// `ramBytesUsed` default.
export inline constexpr int64_t QUERY_DEFAULT_RAM_BYTES_USED = 1024;

/// One kilobyte.
export inline constexpr int64_t ONE_KB = 1024;
/// One megabyte.
export inline constexpr int64_t ONE_MB = ONE_KB * ONE_KB;
/// One gigabyte.
export inline constexpr int64_t ONE_GB = ONE_KB * ONE_MB;

/// A primitive value type whose size memory accounting needs.
///
/// Port of `PrimitiveType` (`util/ram_usage_estimator.rs`).
export enum class PrimitiveType : int32_t {
    Boolean = 0,
    Byte = 1,
    Char = 2,
    Short = 3,
    Int = 4,
    Float = 5,
    Double = 6,
    Long = 7,
};

/// The size of `primitive_type` in bytes.
[[nodiscard]] constexpr int64_t primitive_size(PrimitiveType primitive_type) noexcept {
    switch (primitive_type) {
        case PrimitiveType::Boolean:
            return sizeof(bool);
        case PrimitiveType::Byte:
            return sizeof(uint8_t);
        case PrimitiveType::Char:
            return sizeof(char32_t);
        case PrimitiveType::Short:
            return sizeof(int16_t);
        case PrimitiveType::Int:
            return sizeof(int32_t);
        case PrimitiveType::Float:
            return sizeof(float);
        case PrimitiveType::Double:
            return sizeof(double);
        case PrimitiveType::Long:
            return sizeof(int64_t);
    }
    return 0;
}

namespace detail {

[[nodiscard]] constexpr int64_t saturating_i64(size_t value) noexcept {
    return value > static_cast<size_t>(std::numeric_limits<int64_t>::max())
                   ? std::numeric_limits<int64_t>::max()
                   : static_cast<int64_t>(value);
}

} // namespace detail

/// Retained heap bytes of `capacity` elements of `T`.
///
/// Port of `size_of_array_capacity`: an empty or zero sized type costs nothing.
export template <typename T>
[[nodiscard]] constexpr int64_t size_of_array_capacity(size_t capacity) noexcept {
    if (capacity == 0 || sizeof(T) == 0) {
        return 0;
    }
    return detail::saturating_i64(capacity * sizeof(T));
}

/// Retained heap bytes of the buffer of `values`.
///
/// **Deviation:** Rust's `Vec::capacity()` is not observable in C++, so the
/// element count is used; `std::vector` never over-allocates by much, which is
/// what makes the estimate useful.
export template <typename T>
[[nodiscard]] int64_t size_of_vec(const std::vector<T> &values) noexcept {
    return size_of_array_capacity<T>(values.capacity());
}

/// A lower bound on the retained heap bytes of a `std::deque`.
///
/// **Deviation:** `std::deque` exposes no capacity and allocates in blocks, so
/// the element count is the closest observable measure.
export template <typename T>
[[nodiscard]] int64_t size_of_vec_deque(const std::deque<T> &values) noexcept {
    return size_of_array_capacity<T>(values.size());
}

/// A lower bound on the retained heap bytes of a hash map.
///
/// **Deviation:** Rust's `HashMap::capacity()` counts entries; the C++ analogue
/// is `bucket_count()`, which is what the bucket array holds.
export template <typename K, typename V>
[[nodiscard]] int64_t size_of_hash_map(const std::unordered_map<K, V> &map) noexcept {
    return size_of_array_capacity<std::pair<const K, V>>(map.bucket_count());
}

/// Retained heap bytes of the `std::string` buffer, not counting the object.
export [[nodiscard]] inline int64_t size_of_string(std::string_view text) noexcept {
    return text.empty() ? 0 : detail::saturating_i64(text.size());
}

/// Retained heap bytes of a `std::vector<std::string>`, buffers included.
export [[nodiscard]] inline int64_t size_of_string_vec(const std::vector<std::string> &values) noexcept {
    int64_t size = size_of_vec(values);
    for (const std::string &value : values) {
        size = size > std::numeric_limits<int64_t>::max() - size_of_string(value)
                       ? std::numeric_limits<int64_t>::max()
                       : size + size_of_string(value);
    }
    return size;
}

/// Retained heap bytes of an exposed array of `T`.
export template <typename T>
[[nodiscard]] int64_t size_of_slice(std::span<const T> values) noexcept {
    return size_of_array_capacity<T>(values.size());
}

/// The shallow size of one `T`, i.e. its inline footprint.
export template <typename T>
[[nodiscard]] constexpr int64_t size_of_entry() noexcept {
    return static_cast<int64_t>(sizeof(T));
}

/// `bytes` rendered in human readable units (`GB`, `MB`, `KB` or plain bytes).
///
/// Port of `human_readable_units`: one decimal at most, and a trailing `.0` is
/// dropped.
[[nodiscard]] std::string human_readable_units(int64_t bytes);

} // namespace lucene
