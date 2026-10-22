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

export module lucene_core:bytes_ref_comparator;

import :types;
import :error;

import std;

import :bytes_ref;

namespace lucene {

/// A [`BytesRef`] comparator that also exposes the individual bytes being
/// compared, which lets `StringSorter` apply prefix-sharing optimizations.
///
/// Port of the `BytesRefComparator` trait (`util/bytes_ref_comparator.rs`).
///
/// **Deviation:** the reference implementation's `byte_at`/`compare_with_offset` return `Result`
/// because the Rust `ByteSource` abstraction can fail; here a `BytesRef` always
/// owns contiguous bytes, so the methods are infallible and `-1` is used for
/// "past the end", like Lucene's Java interface.
export class BytesRefComparator {
public:
    virtual ~BytesRefComparator() = default;

    /// The unsigned byte used for comparison at index `i`, or `-1` once all
    /// bytes relevant for the comparison are exhausted.
    ///
    /// May only be called with `i < compared_bytes_count()`.
    [[nodiscard]] virtual int32_t byte_at(const BytesRef &value, size_t i) const = 0;

    /// Compares both values starting at byte `k`.
    [[nodiscard]] virtual int32_t compare_with_offset(const BytesRef &o1, const BytesRef &o2, size_t k) const;

    /// Number of bytes this comparator inspects.
    [[nodiscard]] virtual size_t compared_bytes_count() const = 0;

    /// Full comparison (equivalent to `compare_with_offset(a, b, 0)`).
    [[nodiscard]] virtual int32_t compare(const BytesRef &a, const BytesRef &b) const = 0;
};

/// Natural (unsigned lexicographic) order, matching Lucene's
/// `BytesRefComparator.NATURAL`.  This is the comparator `StringSorter` has
/// optimizations for.
export class NaturalBytesRefComparator final : public BytesRefComparator {
public:
    [[nodiscard]] int32_t byte_at(const BytesRef &value, size_t i) const override;
    [[nodiscard]] int32_t compare_with_offset(const BytesRef &o1, const BytesRef &o2, size_t k) const override;
    [[nodiscard]] size_t compared_bytes_count() const override;
    [[nodiscard]] int32_t compare(const BytesRef &a, const BytesRef &b) const override;
};

/// Type name reported by this comparator (`BYTES_REF_COMPARATOR_TYPE`).
inline constexpr std::string_view BYTES_REF_COMPARATOR_TYPE = "BytesRefComparator";

} // namespace lucene
