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
#include <string>
#include <vector>

module lucene_core:ut.sorter;

import lucene_core;

using namespace lucene;

namespace {

/// Sorts a `std::vector<BytesRef>` slot by slot, which is what the reference implementation's
/// `StringSorterTestImpl` does.
class BytesSorterDelegate final : public StringSorterBase {
public:
    explicit BytesSorterDelegate(std::vector<BytesRef> values) : values_(std::move(values)) {}

    [[nodiscard]] Result<BytesRef> get(size_t i) override { return values_.at(i); }

    [[nodiscard]] VoidResult swap(size_t i, size_t j) override {
        std::swap(values_.at(i), values_.at(j));
        return {};
    }

    [[nodiscard]] const std::vector<BytesRef> &values() const noexcept { return values_; }

private:
    std::vector<BytesRef> values_;
};

/// An `IntroSorter` over `std::vector<int32_t>` slots.
class IntIntroSorter final : public IntroSorter {
public:
    explicit IntIntroSorter(std::vector<int32_t> values) : values_(std::move(values)) {}

    [[nodiscard]] Result<int32_t> compare(size_t i, size_t j) override {
        const int32_t left = values_.at(i);
        const int32_t right = values_.at(j);
        return left < right ? -1 : (left > right ? 1 : 0);
    }

    [[nodiscard]] VoidResult swap(size_t i, size_t j) override {
        std::swap(values_.at(i), values_.at(j));
        return {};
    }

    [[nodiscard]] VoidResult set_pivot(size_t i) override {
        pivot_ = values_.at(i);
        return {};
    }

    [[nodiscard]] Result<int32_t> compare_pivot(size_t j) override {
        const int32_t right = values_.at(j);
        return pivot_ < right ? -1 : (pivot_ > right ? 1 : 0);
    }

    [[nodiscard]] const std::vector<int32_t> &values() const noexcept { return values_; }

private:
    std::vector<int32_t> values_;
    int32_t pivot_{0};
};

/// A radix-sortable key: four bytes compared most significant first.
class UIntRadixDelegate final : public MSBRadixSorterBase {
public:
    explicit UIntRadixDelegate(std::vector<uint32_t> keys) : keys_(std::move(keys)) {}

    [[nodiscard]] Result<int32_t> byte_at(size_t i, size_t k) override {
        if (k >= 4) {
            return -1;
        }
        const uint32_t key = keys_.at(i);
        return static_cast<int32_t>((key >> (8 * (3 - k))) & 0xFF);
    }

    [[nodiscard]] VoidResult swap(size_t i, size_t j) override {
        std::swap(keys_.at(i), keys_.at(j));
        return {};
    }

    [[nodiscard]] const std::vector<uint32_t> &keys() const noexcept { return keys_; }

private:
    std::vector<uint32_t> keys_;
};

std::vector<BytesRef> to_refs(const std::vector<std::string> &texts) {
    std::vector<BytesRef> refs;
    refs.reserve(texts.size());
    for (const auto &text : texts) {
        refs.push_back(BytesRef::from_string(text));
    }
    return refs;
}

std::vector<std::string> to_strings(const std::vector<BytesRef> &refs) {
    std::vector<std::string> texts;
    texts.reserve(refs.size());
    for (const auto &ref : refs) {
        const auto slice = ref.as_slice();
        texts.emplace_back(slice.begin(), slice.end());
    }
    return texts;
}

bool is_sorted_ascending(const std::vector<std::string> &texts) {
    return std::is_sorted(texts.begin(), texts.end());
}

} // namespace

TEST(SorterTest, StringSorterSortsInNaturalOrder) {
    std::vector<std::string> texts = {"delta", "alpha", "charlie", "bravo", "alpha", "echo"};
    auto delegate = std::make_unique<BytesSorterDelegate>(to_refs(texts));
    auto *view = delegate.get();
    NaturalBytesRefComparator comparator;

    StringSorter sorter(std::move(delegate), comparator);
    ASSERT_TRUE(sorter.sort(0, texts.size()).has_value());

    auto actual = to_strings(view->values());
    std::vector<std::string> expected = texts;
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(actual, expected);
}

TEST(SorterTest, StringSorterHandlesEmptyAndSingleRanges) {
    NaturalBytesRefComparator comparator;

    auto empty = std::make_unique<BytesSorterDelegate>(std::vector<BytesRef>{});
    StringSorter empty_sorter(std::move(empty), comparator);
    EXPECT_TRUE(empty_sorter.sort(0, 0).has_value());

    auto single = std::make_unique<BytesSorterDelegate>(to_refs({"only"}));
    auto *view = single.get();
    StringSorter single_sorter(std::move(single), comparator);
    ASSERT_TRUE(single_sorter.sort(0, 1).has_value());
    EXPECT_EQ(to_strings(view->values()), (std::vector<std::string>{"only"}));
}

TEST(SorterTest, StringSorterSortsLongSharedPrefixes) {
    // Keys that share a long prefix exercise the common-prefix path and the
    // introsort fallback for buckets that never diverge.
    std::vector<std::string> texts;
    const std::string shared(40, 'p');
    for (int32_t i = 0; i < 300; ++i) {
        texts.push_back(shared + std::to_string((i * 7919) % 300));
    }
    // Plus one key that is a strict prefix of another, so the "-1" end marker
    // bucket is used.
    texts.push_back(shared);
    texts.push_back(shared + "~");

    auto delegate = std::make_unique<BytesSorterDelegate>(to_refs(texts));
    auto *view = delegate.get();
    NaturalBytesRefComparator comparator;

    StringSorter sorter(std::move(delegate), comparator);
    ASSERT_TRUE(sorter.sort(0, texts.size()).has_value());

    auto actual = to_strings(view->values());
    EXPECT_TRUE(is_sorted_ascending(actual));
    std::vector<std::string> expected = texts;
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(actual, expected);
    EXPECT_EQ(actual.size(), texts.size());
}

TEST(SorterTest, StringSorterSortsAThousandKeys) {
    std::vector<std::string> texts;
    texts.reserve(1000);
    uint32_t state = 12345;
    for (int32_t i = 0; i < 1000; ++i) {
        // A small LCG keeps the test deterministic.
        state = state * 1664525U + 1013904223U;
        const std::string text = "k" + std::to_string(state % 5000);
        texts.push_back(text);
    }

    auto delegate = std::make_unique<BytesSorterDelegate>(to_refs(texts));
    auto *view = delegate.get();
    NaturalBytesRefComparator comparator;

    StringSorter sorter(std::move(delegate), comparator);
    ASSERT_TRUE(sorter.sort(0, texts.size()).has_value());

    auto actual = to_strings(view->values());
    std::vector<std::string> expected = texts;
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(actual, expected);
}

TEST(SorterTest, IntroSorterSortsSmallAndDuplicateHeavyRanges) {
    // Small: insertion sort.
    {
        IntIntroSorter sorter({5, 3, 9, 1, 7});
        ASSERT_TRUE(sorter.sort(0, 5).has_value());
        EXPECT_EQ(sorter.values(), (std::vector<int32_t>{1, 3, 5, 7, 9}));
    }
    // Descending and large: quicksort with the ninther pivot.
    {
        std::vector<int32_t> values;
        for (int32_t i = 200; i > 0; --i) {
            values.push_back(i);
        }
        IntIntroSorter sorter(values);
        ASSERT_TRUE(sorter.sort(0, values.size()).has_value());
        EXPECT_TRUE(std::is_sorted(sorter.values().begin(), sorter.values().end()));
    }
    // Many duplicates: the 3-way partition must not degrade or lose entries.
    {
        std::vector<int32_t> values;
        for (int32_t i = 0; i < 500; ++i) {
            values.push_back(i % 3);
        }
        IntIntroSorter sorter(values);
        ASSERT_TRUE(sorter.sort(0, values.size()).has_value());
        EXPECT_TRUE(std::is_sorted(sorter.values().begin(), sorter.values().end()));
        EXPECT_EQ(sorter.values().size(), 500U);
        // 1 occurs at i = 1, 4, ..., 499: 167 times.
        EXPECT_EQ(std::count(sorter.values().begin(), sorter.values().end(), 1), 167);
    }
    // A sub-range is sorted, the rest is untouched.
    {
        IntIntroSorter sorter({9, 4, 2, 8, 1});
        ASSERT_TRUE(sorter.sort(1, 4).has_value());
        EXPECT_EQ(sorter.values(), (std::vector<int32_t>{9, 2, 4, 8, 1}));
    }
}

TEST(SorterTest, MsbRadixSorterSortsManyKeys) {
    std::vector<uint32_t> keys;
    keys.reserve(1000);
    uint32_t state = 987654321;
    for (int32_t i = 0; i < 1000; ++i) {
        state = state * 1664525U + 1013904223U;
        keys.push_back(state);
    }

    auto delegate = std::make_unique<UIntRadixDelegate>(keys);
    auto *view = delegate.get();
    MSBRadixSorter sorter(4, std::move(delegate));
    ASSERT_TRUE(sorter.sort(0, keys.size()).has_value());

    std::vector<uint32_t> expected = keys;
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(view->keys(), expected);
}

TEST(SorterTest, MsbRadixSorterSortsRepeatedAndShortKeys) {
    // Keys that are equal exercise the "the key ended" bucket, and the small
    // range size forces the introsort fallback.
    std::vector<uint32_t> keys = {7, 7, 7, 1, 2, 2, 65535, 256, 1, 0};
    auto delegate = std::make_unique<UIntRadixDelegate>(keys);
    auto *view = delegate.get();
    MSBRadixSorter sorter(4, std::move(delegate));
    ASSERT_TRUE(sorter.sort(0, keys.size()).has_value());

    std::vector<uint32_t> expected = keys;
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(view->keys(), expected);
}

TEST(SorterTest, SorterRejectsInvertedRanges) {
    IntIntroSorter sorter({1, 2, 3});
    auto sorted = sorter.sort(2, 1);
    ASSERT_FALSE(sorted.has_value());
    EXPECT_EQ(sorted.error().kind(), ErrorKind::kIllegalArgument);
}
