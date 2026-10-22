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

module lucene_core:bytes_ref_comparator.impl;

import :bytes_ref_comparator;
import :types;
import :error;
import :bytes_ref;

import std;

namespace lucene {

namespace {

/// Unsigned lexicographic comparison of two byte ranges.
int32_t compare_bytes(std::span<const uint8_t> a, std::span<const uint8_t> b) noexcept {
    const size_t common = std::min(a.size(), b.size());
    if (common != 0) {
        const int32_t result = std::memcmp(a.data(), b.data(), common);
        if (result != 0) {
            return result < 0 ? -1 : 1;
        }
    }
    if (a.size() == b.size()) {
        return 0;
    }
    return a.size() < b.size() ? -1 : 1;
}

} // namespace

int32_t BytesRefComparator::compare_with_offset(const BytesRef &o1, const BytesRef &o2, size_t k) const {
    for (size_t i = k; i < compared_bytes_count(); ++i) {
        const int32_t b1 = byte_at(o1, i);
        const int32_t b2 = byte_at(o2, i);
        if (b1 != b2) {
            return b1 - b2;
        }
        if (b1 == -1) {
            break;
        }
    }
    return 0;
}

int32_t NaturalBytesRefComparator::byte_at(const BytesRef &value, size_t i) const {
    if (value.length() <= i) {
        return -1;
    }
    return static_cast<int32_t>(value.as_slice()[i]);
}

int32_t NaturalBytesRefComparator::compare_with_offset(const BytesRef &o1, const BytesRef &o2, size_t k) const {
    const auto slice1 = o1.as_slice();
    const auto slice2 = o2.as_slice();
    const size_t start1 = std::min(k, slice1.size());
    const size_t start2 = std::min(k, slice2.size());
    return compare_bytes(slice1.subspan(start1), slice2.subspan(start2));
}

size_t NaturalBytesRefComparator::compared_bytes_count() const {
    // `i32::MAX` in the reference implementation: effectively unbounded.
    return static_cast<size_t>(std::numeric_limits<int32_t>::max());
}

int32_t NaturalBytesRefComparator::compare(const BytesRef &a, const BytesRef &b) const {
    return compare_with_offset(a, b, 0);
}

} // namespace lucene
