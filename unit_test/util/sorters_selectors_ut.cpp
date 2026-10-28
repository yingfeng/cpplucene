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

module;

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <memory>
#include <numeric>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.sorters_selectors;

import lucene_core;

using namespace lucene;

namespace {

/// Deterministic pseudo random values so failures are reproducible.
std::vector<int32_t> make_values(size_t size, uint32_t seed) {
    std::mt19937 engine(seed);
    std::uniform_int_distribution<int32_t> distribution(0, static_cast<int32_t>(size * 3));
    std::vector<int32_t> values(size);
    for (int32_t &value : values) {
        value = distribution(engine);
    }
    return values;
}

/// IntroSelector hooks over a plain `int32_t` array.
class IntHooks final : public IntroSelectorHooks {
public:
    explicit IntHooks(std::span<int32_t> values) : values_(values) {}

    [[nodiscard]] VoidResult set_pivot(size_t i) override {
        pivot_ = values_[i];
        return {};
    }

    [[nodiscard]] Result<int32_t> compare_pivot(size_t j) override {
        return pivot_ < values_[j] ? -1 : (pivot_ > values_[j] ? 1 : 0);
    }

    [[nodiscard]] Result<int32_t> compare(size_t i, size_t j) override {
        return values_[i] < values_[j] ? -1 : (values_[i] > values_[j] ? 1 : 0);
    }

    [[nodiscard]] VoidResult swap(size_t i, size_t j) override {
        std::swap(values_[i], values_[j]);
        return {};
    }

private:
    std::span<int32_t> values_;
    int32_t pivot_{0};
};

/// RadixSelector hooks over byte strings; `byte_at` returns -1 past the end.
class ByteKeyHooks final : public RadixSelectorHooks {
public:
    explicit ByteKeyHooks(std::span<std::vector<uint8_t>> keys) : keys_(keys) {}

    [[nodiscard]] Result<int32_t> byte_at(size_t i, size_t k) override {
        if (k >= keys_[i].size()) {
            return -1;
        }
        return static_cast<int32_t>(keys_[i][k]);
    }

    [[nodiscard]] VoidResult swap(size_t i, size_t j) override {
        std::swap(keys_[i], keys_[j]);
        return {};
    }

private:
    std::span<std::vector<uint8_t>> keys_;
};

std::vector<uint8_t> key_of(const std::string &text, int32_t suffix) {
    std::vector<uint8_t> key(text.begin(), text.end());
    // Append a terminator-free suffix so keys of equal prefix stay distinct.
    key.push_back(static_cast<uint8_t>(suffix));
    return key;
}

/// StableMSBRadixSorter hooks over byte keys.
///
/// The permutation lives in test owned storage so that it can be inspected
/// after the sort; `slots[i]` is the index of the key now in slot `i`.
class StableByteKeyHooks final : public StableMSBRadixSorterHooks {
public:
    StableByteKeyHooks(const std::vector<std::vector<uint8_t>> &keys,
                       std::vector<size_t> &slots,
                       std::vector<size_t> &scratch)
        : keys_(keys), slots_(slots), scratch_(scratch) {}

    [[nodiscard]] Result<int32_t> byte_at(size_t i, size_t k) override {
        const auto &key = keys_[slots_[i]];
        if (k >= key.size()) {
            return -1;
        }
        return static_cast<int32_t>(key[k]);
    }

    [[nodiscard]] VoidResult swap(size_t i, size_t j) override {
        std::swap(slots_[i], slots_[j]);
        return {};
    }

    [[nodiscard]] VoidResult set_pivot(size_t i) override {
        pivot_index_ = i;
        return {};
    }

    [[nodiscard]] Result<int32_t> compare_pivot(size_t j) override { return compare(pivot_index_, j); }

    [[nodiscard]] Result<int32_t> compare(size_t i, size_t j) override {
        const auto &left = keys_[slots_[i]];
        const auto &right = keys_[slots_[j]];
        if (left < right) {
            return -1;
        }
        return left > right ? 1 : 0;
    }

    [[nodiscard]] VoidResult save(size_t i, size_t j) override {
        scratch_[j] = slots_[i];
        return {};
    }

    [[nodiscard]] VoidResult restore(size_t i, size_t j) override {
        for (size_t slot = i; slot < j; ++slot) {
            slots_[slot] = scratch_[slot];
        }
        return {};
    }

private:
    const std::vector<std::vector<uint8_t>> &keys_;
    std::vector<size_t> &slots_;
    std::vector<size_t> &scratch_;
    size_t pivot_index_{0};
};

// ---------------- IntroSelector ----------------

TEST(IntroSelectorTest, SelectsTheKthValue) {
    for (const size_t size : {size_t{1}, size_t{2}, size_t{3}, size_t{4}, size_t{7}, size_t{39}, size_t{40}, size_t{41},
                              size_t{997}}) {
        auto values = make_values(size, static_cast<uint32_t>(size));
        auto expected = values;
        std::sort(expected.begin(), expected.end());

        for (const size_t k : {size_t{0}, size / 2, size - 1}) {
            auto working = values;
            IntHooks hooks(working);
            IntroSelector selector(hooks);
            ASSERT_TRUE(selector.select(0, size, k).has_value()) << "size=" << size << " k=" << k;
            EXPECT_EQ(working[k], expected[k]) << "size=" << size << " k=" << k;
            // Partition invariant: everything before k is <=, everything after >=.
            for (size_t i = 0; i < k; ++i) {
                EXPECT_LE(working[i], working[k]) << "size=" << size << " k=" << k << " i=" << i;
            }
            for (size_t i = k + 1; i < size; ++i) {
                EXPECT_GE(working[i], working[k]) << "size=" << size << " k=" << k << " i=" << i;
            }
        }
    }
}

TEST(IntroSelectorTest, RejectsBadArguments) {
    auto values = make_values(10, 7);
    IntHooks hooks(values);
    IntroSelector selector(hooks);
    EXPECT_FALSE(selector.select(0, 10, 10).has_value());
    EXPECT_FALSE(selector.select(5, 10, 4).has_value());
    EXPECT_TRUE(selector.check_args(0, 10, 9).has_value());
}

TEST(IntroSelectorTest, ShufflesWhenTheDepthBudgetIsExhausted) {
    // A zero budget forces the shuffle branch on every level.
    auto values = make_values(500, 11);
    auto expected = values;
    std::sort(expected.begin(), expected.end());
    IntHooks hooks(values);
    IntroSelector selector(hooks);
    ASSERT_TRUE(selector.select_in_intro(0, values.size(), 250, 0).has_value());
    EXPECT_EQ(values[250], expected[250]);
}

TEST(IntroSelectorTest, MedianAndSortThree) {
    auto values = std::vector<int32_t>{9, 1, 5, 3};
    IntHooks hooks(values);
    IntroSelector selector(hooks);

    auto median = selector.median(0, 1, 2);
    ASSERT_TRUE(median.has_value());
    EXPECT_EQ(median.value(), 2U); // value 5 sits at index 2

    ASSERT_TRUE(selector.sort3(0).has_value());
    EXPECT_EQ(values[0], 1);
    EXPECT_EQ(values[1], 5);
    EXPECT_EQ(values[2], 9);
}

// ---------------- RadixSelector ----------------

TEST(RadixSelectorTest, SelectsTheKthByteKey) {
    std::vector<std::vector<uint8_t>> keys;
    keys.reserve(400);
    for (size_t i = 0; i < 400; ++i) {
        // Keys share long prefixes so the common-prefix path is exercised.
        keys.push_back(key_of("prefix-" + std::string(1, static_cast<char>('a' + (i % 5))), static_cast<int32_t>(i % 17)));
    }
    auto expected = keys;
    std::sort(expected.begin(), expected.end());

    for (const size_t k : {size_t{0}, size_t{123}, size_t{399}}) {
        auto working = keys;
        ByteKeyHooks hooks(working);
        RadixSelector selector(9, hooks);
        ASSERT_TRUE(selector.select(0, working.size(), k).has_value()) << "k=" << k;
        EXPECT_EQ(working[k], expected[k]) << "k=" << k;
        for (size_t i = 0; i < k; ++i) {
            EXPECT_LE(working[i], working[k]) << "k=" << k << " i=" << i;
        }
        for (size_t i = k + 1; i < working.size(); ++i) {
            EXPECT_GE(working[i], working[k]) << "k=" << k << " i=" << i;
        }
    }
}

TEST(RadixSelectorTest, FallsBackForNarrowRanges) {
    std::vector<std::vector<uint8_t>> keys;
    for (size_t i = 0; i < 40; ++i) {
        keys.push_back(key_of("k", static_cast<int32_t>(39 - i)));
    }
    auto expected = keys;
    std::sort(expected.begin(), expected.end());
    ByteKeyHooks hooks(keys);
    RadixSelector selector(3, hooks);
    ASSERT_TRUE(selector.select(0, keys.size(), 20).has_value());
    EXPECT_EQ(keys[20], expected[20]);
}

// ---------------- LSBRadixSorter ----------------

TEST(LSBRadixSorterTest, SortsNonNegativeValues) {
    auto values = make_values(1000, 23);
    for (int32_t &value : values) {
        value &= 0x7FFFFFFF;
    }
    auto expected = values;
    std::sort(expected.begin(), expected.end());

    LSBRadixSorter sorter;
    ASSERT_TRUE(sorter.sort(32, values, values.size()).has_value());
    EXPECT_EQ(values, expected);

    // Reusable: sorting again with the same instance keeps working.
    std::reverse(values.begin(), values.end());
    ASSERT_TRUE(sorter.sort(32, values, values.size()).has_value());
    EXPECT_EQ(values, expected);
}

TEST(LSBRadixSorterTest, SmallAndConstantInputs) {
    LSBRadixSorter sorter;
    std::vector<int32_t> few{5, 3, 9, 1};
    ASSERT_TRUE(sorter.sort(32, few, few.size()).has_value());
    EXPECT_EQ(few, (std::vector<int32_t>{1, 3, 5, 9}));

    std::vector<int32_t> same(100, 7);
    ASSERT_TRUE(sorter.sort(32, same, same.size()).has_value());
    EXPECT_EQ(same, std::vector<int32_t>(100, 7));

    std::vector<int32_t> empty;
    ASSERT_TRUE(sorter.sort(32, empty, 0).has_value());
    EXPECT_FALSE(sorter.sort(32, empty, 1).has_value());
}

// ---------------- TimSorter / ArrayTimSorter ----------------

namespace {

struct Sortable {
    int32_t key;
    int32_t id;
};

/// Orders by `key` only, so stability shows up in `id`.
Result<int32_t> compare_sortable(const Sortable &a, const Sortable &b) {
    return a.key < b.key ? -1 : (a.key > b.key ? 1 : 0);
}

Result<int32_t> compare_int(const int32_t &a, const int32_t &b) { return a < b ? -1 : (a > b ? 1 : 0); }

} // namespace

TEST(TimSorterTest, SortsWithAndWithoutTemporaryStorage) {
    for (const size_t max_temp : {size_t{0}, size_t{8}, size_t{4096}}) {
        auto values = make_values(600, 31);
        for (int32_t &value : values) {
            value %= 97; // plenty of duplicates
        }
        auto expected = values;
        std::sort(expected.begin(), expected.end());

        ArrayTimSorter<int32_t, Result<int32_t> (*)(const int32_t &, const int32_t &)> hooks(
            values, compare_int, max_temp);
        auto sorter = hooks.tim_sorter(max_temp);
        ASSERT_TRUE(sorter->sort(0, values.size()).has_value()) << "max_temp=" << max_temp;
        EXPECT_EQ(values, expected) << "max_temp=" << max_temp;
    }
}

TEST(TimSorterTest, IsStable) {
    std::vector<Sortable> values;
    for (int32_t i = 0; i < 500; ++i) {
        values.push_back(Sortable{i % 7, i});
    }

    ArrayTimSorter<Sortable, Result<int32_t> (*)(const Sortable &, const Sortable &)> hooks(
        values, compare_sortable, 64);
    auto sorter = hooks.tim_sorter(64);
    ASSERT_TRUE(sorter->sort(0, values.size()).has_value());

    // Within one key the ids must still be ascending.
    int32_t previous_key = values.front().key;
    int32_t previous_id = -1;
    for (const Sortable &value : values) {
        ASSERT_LE(previous_key, value.key);
        if (value.key != previous_key) {
            previous_key = value.key;
            previous_id = -1;
        }
        ASSERT_GT(value.id, previous_id) << "key=" << value.key;
        previous_id = value.id;
    }
}

TEST(TimSorterTest, SortsSubRangeAndTinyInputs) {
    auto values = make_values(200, 41);
    auto expected = values;
    std::sort(expected.begin() + 50, expected.begin() + 150);

    ArrayTimSorter<int32_t, Result<int32_t> (*)(const int32_t &, const int32_t &)> hooks(values, compare_int, 32);
    auto sorter = hooks.tim_sorter(32);
    ASSERT_TRUE(sorter->sort(50, 150).has_value());
    EXPECT_EQ(values, expected);

    ASSERT_TRUE(sorter->sort(0, 0).has_value());
    ASSERT_TRUE(sorter->sort(0, 1).has_value());
    EXPECT_FALSE(sorter->sort(5, 4).has_value());
}

TEST(TimSorterTest, MinRunStaysInRange) {
    EXPECT_GE(tim_min_run(1000), TIM_MIN_RUN);
    EXPECT_LE(tim_min_run(1000), TIM_THRESHOLD);
    EXPECT_EQ(tim_min_run(64), 32U);
    EXPECT_EQ(tim_min_run(128), 32U);
}

// ---------------- ArrayIntroSorter ----------------

TEST(ArrayIntroSorterTest, SortsByComparator) {
    auto values = make_values(300, 53);
    auto expected = values;
    std::sort(expected.begin(), expected.end());

    ArrayIntroSorter<int32_t, Result<int32_t> (*)(const int32_t &, const int32_t &)> sorter(values, compare_int);
    ASSERT_TRUE(sorter.sort(0, values.size()).has_value());
    EXPECT_EQ(values, expected);

    ASSERT_TRUE(sorter.sort(0, 0).has_value());
    EXPECT_FALSE(sorter.sort(2, 1).has_value());
}

// ---------------- StableMSBRadixSorter ----------------

namespace {

/// Sorts `keys` with a stable MSB radix sort and returns the permutation.
std::vector<size_t> stable_sort_keys(const std::vector<std::vector<uint8_t>> &keys, size_t max_length) {
    std::vector<size_t> slots(keys.size());
    std::iota(slots.begin(), slots.end(), size_t{0});
    std::vector<size_t> scratch(keys.size());
    MSBRadixSorter sorter(max_length, std::make_unique<StableMSBRadixSorter>(
                                              std::make_unique<StableByteKeyHooks>(keys, slots, scratch), max_length));
    EXPECT_TRUE(sorter.sort(0, keys.size()).has_value());
    return slots;
}

void expect_stably_sorted(const std::vector<std::vector<uint8_t>> &keys, const std::vector<size_t> &slots) {
    for (size_t i = 1; i < slots.size(); ++i) {
        ASSERT_LE(keys[slots[i - 1]], keys[slots[i]]) << "i=" << i;
        if (keys[slots[i - 1]] == keys[slots[i]]) {
            ASSERT_LT(slots[i - 1], slots[i]) << "not stable at i=" << i;
        }
    }
}

} // namespace

TEST(StableMSBRadixSorterTest, SortsEqualKeysInInputOrder) {
    // Only six distinct keys: the buckets fall back to the stable merge sort.
    std::vector<std::vector<uint8_t>> keys;
    for (size_t i = 0; i < 600; ++i) {
        keys.push_back(key_of("key-" + std::string(1, static_cast<char>('a' + (i % 6))), 0));
    }
    const auto slots = stable_sort_keys(keys, 5);
    ASSERT_EQ(slots.size(), keys.size());
    expect_stably_sorted(keys, slots);
}

TEST(StableMSBRadixSorterTest, SortsManyDistinctKeysStably) {
    // Many distinct keys, sharing prefixes, so the radix reorder path runs.
    std::vector<std::vector<uint8_t>> keys;
    for (size_t i = 0; i < 800; ++i) {
        keys.push_back(key_of("common-prefix-" + std::to_string((i * 37) % 199), static_cast<int32_t>(i % 3)));
    }
    const auto slots = stable_sort_keys(keys, 24);
    ASSERT_EQ(slots.size(), keys.size());
    expect_stably_sorted(keys, slots);
}

TEST(StableMSBRadixSorterTest, SortsEmptyAndSingle) {
    std::vector<std::vector<uint8_t>> keys;
    EXPECT_TRUE(stable_sort_keys(keys, 4).empty());

    keys.push_back(key_of("only", 1));
    const auto slots = stable_sort_keys(keys, 5);
    ASSERT_EQ(slots.size(), 1U);
    EXPECT_EQ(slots[0], 0U);
}

} // namespace
