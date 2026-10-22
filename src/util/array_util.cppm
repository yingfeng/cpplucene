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

export module lucene_core:array_util;

import :types;
import std;

import :error;

namespace lucene {

/// Port of the growth/allocation helpers of `core/util/array_util.rs`.
///
/// Only the subset required by the foundation layer is ported so far; sorting,
/// radix parse and the `i32` helpers follow with the codec port.
export struct ArrayUtil {
    /// Maximum number of elements supported by Lucene's array-oriented APIs.
    static constexpr size_t MAX_ARRAY_LENGTH = static_cast<size_t>(std::numeric_limits<int32_t>::max());

    /// Returns a vector length greater than or equal to `min_target_size`,
    /// over-allocating exponentially (by one eighth, at least three elements).
    [[nodiscard]] static Result<size_t> oversize(size_t min_target_size, size_t bytes_per_element);

    /// Grows to exactly `new_length`, constructing added elements with `init`.
    template <class T, class F>
    [[nodiscard]] static VoidResult grow_exact(std::vector<T> &vec, size_t new_length, F init) {
        const size_t current_length = vec.size();
        if (new_length > current_length) {
            vec.reserve(new_length);
            vec.resize(new_length, init());
        } else if (new_length < current_length) {
            return err(LuceneError::array_index_out_of_bounds(
                std::format("new_length: {} is less than current_length: {}", new_length, current_length)));
        }
        return {};
    }

    /// Reserves space for at least `min_size` elements without changing the length.
    template <class T>
    [[nodiscard]] static VoidResult grow_capacity(std::vector<T> &vec, size_t min_size) {
        // Only use oversize for its size checks, not as a second growth policy.
        if (auto r = oversize(min_size, sizeof(T)); !r) {
            return std::unexpected(std::move(r.error()));
        }
        vec.reserve(min_size);
        return {};
    }

    /// Ensures at least `min_size` initialized elements, preserving existing contents.
    template <class T, class F>
    [[nodiscard]] static VoidResult grow_with(std::vector<T> &vec, size_t min_size, F init) {
        if (vec.size() < min_size) {
            if (auto r = grow_capacity(vec, min_size); !r) {
                return std::unexpected(std::move(r.error()));
            }
            vec.resize(min_size, init());
        }
        return {};
    }

    /// Ensures at least `min_size` initialized elements for scratch use.
    /// Existing contents may be discarded when a new allocation is needed.
    template <class T, class F>
    [[nodiscard]] static VoidResult grow_no_copy(std::vector<T> &vec, size_t min_size, F init) {
        if (vec.size() < min_size) {
            auto new_capacity = oversize(min_size, sizeof(T));
            if (!new_capacity) {
                return std::unexpected(std::move(new_capacity.error()));
            }
            if (vec.capacity() >= min_size) {
                vec.resize(min_size, init());
            } else {
                std::vector<T> replacement;
                replacement.reserve(new_capacity.value());
                replacement.resize(min_size, init());
                vec = std::move(replacement);
            }
        }
        return {};
    }

    /// Returns the hash of the chars in the range `[start, end)`.
    [[nodiscard]] static int32_t hash_code(std::span<const char32_t> array, size_t start, size_t end);
};

} // namespace lucene
