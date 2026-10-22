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

export module lucene_core:packed_long_values;

import :types;
import std;

import :error;
import :packed64;

namespace lucene {

export class PackedLongValues;

/// Iterates a [`PackedLongValues`] block by block, decoding each block once.
export class PackedLongValuesIterator {
public:
    explicit PackedLongValuesIterator(const PackedLongValues &values);

    [[nodiscard]] Result<bool> has_next() const;
    [[nodiscard]] Result<int64_t> next();

private:
    [[nodiscard]] VoidResult ensure_decoded();

    const PackedLongValues *values_{nullptr};
    std::vector<int64_t> block_;
    size_t block_index_{0};
    size_t index_{0};
};

/// A growable array of `long` values stored compactly in memory.
///
/// Port of `util/packed/packed_long_values.rs`.  Values are accumulated by a
/// [`PackedLongValues::Builder`] and packed into pages of `page_size` values
/// once a page is full.
///
/// **Deviation:** the reference implementation (like Lucene) has three flavours
/// (`PackedLongValues`, `DeltaPackedLongValues`, `MonotonicLongValues`) chosen
/// by the builder factory.  This port keeps the two behaviours that matter for
/// correctness -- absolute values and a per-page `min` offset (delta) -- and
/// uses the delta layout for the monotonic flavour as well.  The layout never
/// reaches the index files, so only the compression ratio differs.
export class PackedLongValues {
public:
    /// Values per page.
    static constexpr size_t DEFAULT_PAGE_SIZE = 256;

    PackedLongValues() = default;

    [[nodiscard]] int64_t size() const noexcept { return static_cast<int64_t>(size_); }
    [[nodiscard]] bool is_empty() const noexcept { return size_ == 0; }

    /// The value at `index`.
    [[nodiscard]] Result<int64_t> get(size_t index) const;

    /// Decodes `block` into `dest`, returning the number of values decoded.
    [[nodiscard]] Result<size_t> decode_block(size_t block, std::vector<int64_t> &dest) const;

    [[nodiscard]] std::unique_ptr<PackedLongValuesIterator> iterator() const;

    /// Bytes held by the packed pages.
    [[nodiscard]] int64_t ram_bytes_used() const;

    [[nodiscard]] size_t page_size() const noexcept { return page_size_; }
    [[nodiscard]] size_t page_count() const noexcept { return pages_.size(); }

    /// Accumulates values and packs them into a [`PackedLongValues`].
    ///
    /// Port of `PackedLongValuesBuilder` (plus its delta/monotonic subclasses).
    class Builder {
    public:
        /// `delta` selects the per-page `min` layout (the reference implementation's delta and
        /// monotonic builders); `false` stores absolute values.
        Builder(size_t page_size, int32_t bits_per_value, int32_t initial_page_count, bool delta);

        explicit Builder(size_t page_size = DEFAULT_PAGE_SIZE)
                : Builder(page_size, 64, 0, false) {}

        [[nodiscard]] VoidResult add(int64_t value);
        [[nodiscard]] int64_t size() const noexcept { return static_cast<int64_t>(size_); }
        [[nodiscard]] int32_t bits_per_value() const noexcept { return bits_per_value_; }

        /// Packs the pending values and returns the finished array.
        ///
        /// Like the reference implementation's `build`, this consumes the builder: it refuses to run
        /// twice.
        [[nodiscard]] Result<PackedLongValues> build();

    private:
        [[nodiscard]] VoidResult pack(std::vector<int64_t> &pending, size_t num_values);

        size_t page_size_;
        int32_t bits_per_value_;
        bool delta_;
        /// The packed values of all finished pages (already offset by `mins_`
        /// when `delta_` is true).
        std::vector<int64_t> values_;
        /// The per-page minimum, empty when `delta_` is false.
        std::vector<int64_t> mins_;
        std::vector<int64_t> pending_;
        size_t size_{0};
        /// Values already packed into `values_`.
        size_t num_values_{0};
        size_t page_count_{0};
        bool built_{false};
    };

private:
    struct Page {
        Packed64 data;
        int64_t base{0};
        size_t count{0};

        Page(Packed64 data_, int64_t base_, size_t count_)
                : data(std::move(data_)), base(base_), count(count_) {}
    };

    friend class PackedLongValuesIterator;

    size_t page_size_{DEFAULT_PAGE_SIZE};
    size_t size_{0};
    std::vector<Page> pages_;
};

} // namespace lucene
