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

export module lucene_core:paged_mutable;

import :types;
import std;

import :error;
import :packed64;
import :packed_ints;

namespace lucene {

/// A mutable array of fixed-width packed integers, split into pages.
///
/// Port of `AbstractPagedMutable` + `PagedMutable`
/// (`util/packed/abstract_paged_mutable.rs`, `paged_mutable.rs`), merged into one
/// class: every page is a [`Packed64`], so the values really are packed and the
/// memory report is the sum of the pages.
export class PagedMutable {
public:
    /// A zeroed array of `size` values of `bits_per_value` bits.
    ///
    /// `acceptable_overhead_ratio` is accepted for signature compatibility; the
    /// port always uses the compact in-memory layout, so it changes nothing.
    [[nodiscard]] static Result<PagedMutable> create(size_t size, int32_t bits_per_value,
                                                     float acceptable_overhead_ratio = PackedInts::DEFAULT);

    [[nodiscard]] int64_t get(size_t index) const;
    [[nodiscard]] VoidResult set(size_t index, int64_t value);

    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] int32_t bits_per_value() const noexcept { return bits_per_value_; }
    /// Values one page holds.
    [[nodiscard]] size_t page_size() const noexcept { return page_size_; }

    /// A copy grown (or shrunk) to `new_size`, keeping the values that fit.
    ///
    /// Port of `AbstractPagedMutable::grow`; the values past the new size are lost
    /// when it shrinks, as in the reference implementation.
    [[nodiscard]] Result<PagedMutable> resize(size_t new_size) const;

    /// Sets every value of `[from, to)` to `value`.
    [[nodiscard]] VoidResult fill(size_t from, size_t to, int64_t value);

    [[nodiscard]] int64_t ram_bytes_used() const;

private:
    PagedMutable(size_t page_size, int32_t bits_per_value, size_t size, std::vector<Packed64> pages)
        : page_size_(page_size), bits_per_value_(bits_per_value), size_(size), pages_(std::move(pages)) {}

    size_t page_size_{0};
    int32_t bits_per_value_{0};
    size_t size_{0};
    std::vector<Packed64> pages_;
};

/// A mutable array whose width grows with the values written into it.
///
/// Port of `PagedGrowableWriter` (`util/packed/paged_growable_writer.rs`): the
/// array starts at one bit per value and re-pages itself with a wider layout as
/// soon as a value does not fit.
export class PagedGrowableWriter {
public:
    /// An empty array of `bits_per_value` bits per value.
    [[nodiscard]] static Result<PagedGrowableWriter> create(size_t size, int32_t start_bits_per_value,
                                                            float acceptable_overhead_ratio = PackedInts::DEFAULT);

    [[nodiscard]] int64_t get(size_t index) const { return values_.get(index); }
    /// Stores `value`, widening the layout when it does not fit.
    [[nodiscard]] VoidResult set(size_t index, int64_t value);

    [[nodiscard]] size_t size() const noexcept { return values_.size(); }
    [[nodiscard]] int32_t bits_per_value() const noexcept { return values_.bits_per_value(); }

    /// A copy grown (or shrunk) to `new_size`.
    [[nodiscard]] Result<PagedGrowableWriter> resize(size_t new_size) const;

    [[nodiscard]] int64_t ram_bytes_used() const { return values_.ram_bytes_used(); }

private:
    PagedGrowableWriter(PagedMutable values, float acceptable_overhead_ratio)
        : values_(std::move(values)), acceptable_overhead_ratio_(acceptable_overhead_ratio) {}

    PagedMutable values_;
    float acceptable_overhead_ratio_{PackedInts::DEFAULT};
};

} // namespace lucene
