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

export module lucene_core:tim_sorter;

import :types;
import std;

import :error;
import :sorter;

namespace lucene {

/// The extra operations a [`TimSorter`] needs besides those of [`Sorter`].
///
/// Port of the `TimSorterBase` trait (`util/tim_sorter.rs`).
export class TimSorterHooks : public Sorter {
public:
    /// Copies slot `src` into slot `dest`.
    [[nodiscard]] virtual VoidResult copy(size_t src, size_t dest) = 0;

    /// Saves `len` slots starting at `i` into the temporary storage.
    [[nodiscard]] virtual VoidResult save(size_t i, size_t len) = 0;

    /// Copies scratch slot `tmp` into array slot `slot`.
    ///
    /// **Deviation:** the reference names these `(i, j)` in the opposite order
    /// in its trait documentation; every call site there passes the scratch
    /// index first, which is what this signature encodes.
    virtual void restore(size_t tmp, size_t slot) = 0;

    /// Compares saved slot `i` with slot `j` of the array being sorted.
    [[nodiscard]] virtual Result<int32_t> compare_saved(size_t i, size_t j) const = 0;
};

/// Length of the first natural run TimSort considers.
export inline constexpr size_t TIM_MIN_RUN = 32;
/// Ranges at most this long are sorted with a binary sort.
export inline constexpr size_t TIM_THRESHOLD = 64;
/// Depth of the run stack (`STACK_SIZE + 1` entries).
export inline constexpr size_t TIM_STACK_SIZE = 49;
/// Consecutive wins before the merge switches to galloping.
export inline constexpr size_t TIM_MIN_GALLOP = 7;

/// Computes the minimum run length for `length` (at least [`TIM_MIN_RUN`]).
[[nodiscard]] size_t tim_min_run(size_t length);

/// TimSort, a stable merge sort that exploits natural runs.
///
/// Port of `util/tim_sorter.rs`.  Small ranges are sorted with a binary sort,
/// merges use a temporary buffer when the shorter side fits in
/// `max_temp_slots` and rotate in place otherwise; only the buffered merges
/// gallop.
export class TimSorter final : public Sorter {
public:
    TimSorter(size_t max_temp_slots, TimSorterHooks &delegate);

    [[nodiscard]] VoidResult sort(size_t from, size_t to) override;

    [[nodiscard]] Result<int32_t> compare(size_t i, size_t j) override { return delegate_.compare(i, j); }
    [[nodiscard]] VoidResult swap(size_t i, size_t j) override { return delegate_.swap(i, j); }
    [[nodiscard]] VoidResult set_pivot(size_t i) override { return delegate_.set_pivot(i); }
    [[nodiscard]] Result<int32_t> compare_pivot(size_t j) override { return delegate_.compare_pivot(j); }

    /// Merges the run starting at index `n` with the next one.
    [[nodiscard]] VoidResult merge_at(size_t n);
    /// Restores the TimSort run length invariants.
    [[nodiscard]] VoidResult ensure_invariants();
    /// Merges every remaining run.
    [[nodiscard]] VoidResult exhaust_stack();
    /// Starts a new sort over `[from, to)`.
    void reset(size_t from, size_t to);

    /// Merges `[lo, mid)` with `[mid, hi)`, using the temporary storage when
    /// the shorter side fits.
    [[nodiscard]] VoidResult merge(size_t lo, size_t mid, size_t hi);
    [[nodiscard]] VoidResult merge_lo(size_t lo, size_t mid, size_t hi);
    [[nodiscard]] VoidResult merge_hi(size_t lo, size_t mid, size_t hi);

    [[nodiscard]] Result<size_t> lower_saved(size_t from, size_t to, size_t val) const;
    [[nodiscard]] Result<size_t> upper_saved(size_t from, size_t to, size_t val) const;
    [[nodiscard]] Result<size_t> lower_saved3(size_t from, size_t to, size_t val) const;
    [[nodiscard]] Result<size_t> upper_saved3(size_t from, size_t to, size_t val) const;

    [[nodiscard]] size_t min_run() const noexcept { return min_run_; }
    [[nodiscard]] size_t stack_size() const noexcept { return stack_size_; }

protected:
    [[nodiscard]] VoidResult do_rotate(size_t lo, size_t mid, size_t hi) override;

private:
    [[nodiscard]] size_t run_len(size_t i) const { return run_ends_[stack_size_ - i] - run_ends_[stack_size_ - i - 1]; }
    [[nodiscard]] size_t run_base(size_t i) const { return run_ends_[stack_size_ - i - 1]; }
    [[nodiscard]] size_t run_end(size_t i) const { return run_ends_[stack_size_ - i]; }
    void set_run_end(size_t i, size_t run_end) { run_ends_[stack_size_ - i] = run_end; }
    void push_run_len(size_t len);

    /// Sorts the next natural run and returns its length.
    [[nodiscard]] Result<size_t> next_run();

    size_t max_temp_slots_;
    size_t min_run_{0};
    size_t to_{0};
    size_t stack_size_{0};
    std::array<size_t, TIM_STACK_SIZE + 1> run_ends_{};
    TimSorterHooks &delegate_;
};

} // namespace lucene
