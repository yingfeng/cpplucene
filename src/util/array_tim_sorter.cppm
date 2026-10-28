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

export module lucene_core:array_tim_sorter;

import :types;
import std;

import :error;
import :sorter;
import :tim_sorter;

namespace lucene {

/// A [`TimSorterHooks`] over a slice of copyable values.
///
/// Port of `util/array_tim_sorter.rs`.  `Compare` is any callable taking two
/// `const T &` and returning `Result<int32_t>`.
///
/// Stable.
export template <typename T, typename Compare>
class ArrayTimSorter final : public TimSorterHooks {
public:
    /// `max_temp_slots` bounds the merge buffer; merges that would need more
    /// rotate in place instead.
    ArrayTimSorter(std::span<T> array, Compare compare, size_t max_temp_slots)
        : array_(array), compare_(std::move(compare)), tmp_(max_temp_slots) {}

    /// The [`TimSorter`] that drives this adapter.
    [[nodiscard]] std::unique_ptr<TimSorter> tim_sorter(size_t max_temp_slots) {
        return std::make_unique<TimSorter>(max_temp_slots, *this);
    }

    // ---- Sorter ----

    [[nodiscard]] Result<int32_t> compare(size_t i, size_t j) override { return compare_(array_[i], array_[j]); }

    [[nodiscard]] VoidResult swap(size_t i, size_t j) override {
        std::swap(array_[i], array_[j]);
        return {};
    }

    [[nodiscard]] VoidResult set_pivot(size_t i) override {
        pivot_index_ = i;
        return {};
    }

    [[nodiscard]] Result<int32_t> compare_pivot(size_t j) override { return compare(pivot_index_, j); }

    // ---- TimSorterHooks ----

    [[nodiscard]] VoidResult copy(size_t src, size_t dest) override {
        array_[dest] = array_[src];
        return {};
    }

    [[nodiscard]] VoidResult save(size_t i, size_t len) override {
        if (len > tmp_.size()) {
            return err(LuceneError::illegal_state(
                std::format("temporary storage holds {} slots but {} were requested", tmp_.size(), len)));
        }
        std::copy_n(array_.begin() + static_cast<std::ptrdiff_t>(i), len, tmp_.begin());
        return {};
    }

    void restore(size_t tmp, size_t slot) override { array_[slot] = tmp_[tmp]; }

    [[nodiscard]] Result<int32_t> compare_saved(size_t i, size_t j) const override { return compare_(tmp_[i], array_[j]); }

private:
    std::span<T> array_;
    mutable Compare compare_;
    std::vector<T> tmp_;
    size_t pivot_index_{0};
};

} // namespace lucene
