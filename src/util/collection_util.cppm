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

export module lucene_core:collection_util;

import :types;
import :error;

import std;

namespace lucene {

/// Sorting and collection helpers.  Port of `util/collection_util.rs`.
///
/// **Deviation:** rlucene hand-ports Lucene's `IntroSorter`/`TimSorter` because
/// Rust's `sort` is not customizable per comparison; C++ already has an
/// introsort (`std::sort`) and a stable merge/timsort hybrid
/// (`std::stable_sort`), so [`intro_sort`] and [`tim_sort`] forward to those.
/// Comparators follow the C++ convention (return `<0`, `0`, `>0`) rather than
/// the `Result<int>` of the Rust `Comparator` trait, because a C++ comparator
/// has no error channel and must stay `noexcept`-friendly.
export struct CollectionUtil {
    /// Sorts `list` with `comp` (single-threaded introsort).
    template <class T, class Compare>
    static void intro_sort(std::span<T> list, Compare comp) {
        if (list.size() <= 1) {
            return;
        }
        std::sort(list.begin(), list.end(), [&comp](const T &a, const T &b) { return comp(a, b) < 0; });
    }

    /// Sorts `list` in natural (`operator<` / `<=>`) order.
    template <class T>
    static void intro_sort(std::span<T> list) {
        if (list.size() <= 1) {
            return;
        }
        std::sort(list.begin(), list.end());
    }

    /// Stable sort of `list` with `comp`.
    template <class T, class Compare>
    static void tim_sort(std::span<T> list, Compare comp) {
        if (list.size() <= 1) {
            return;
        }
        std::stable_sort(list.begin(), list.end(), [&comp](const T &a, const T &b) { return comp(a, b) < 0; });
    }

    /// Stable sort of `list` in natural order.
    template <class T>
    static void tim_sort(std::span<T> list) {
        if (list.size() <= 1) {
            return;
        }
        std::stable_sort(list.begin(), list.end());
    }

    /// A hash map sized for `size` entries without rehashing.
    template <class K, class V>
    [[nodiscard]] static std::unordered_map<K, V> new_hashmap(size_t size) {
        std::unordered_map<K, V> map;
        map.reserve(size);
        return map;
    }
};

} // namespace lucene
