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

#include <memory>
#include <optional>
#include <string>
#include <vector>

module lucene_core:ut.merged_iterator;

import lucene_core;

using namespace lucene;

namespace {

/// A sorted in-memory source, so the tests do not need a real index.
template <typename T>
class VectorSource final : public IteratorExt<T> {
public:
    explicit VectorSource(std::vector<T> items) : items_(std::move(items)) {}

    [[nodiscard]] Result<bool> has_next() override { return i_ < items_.size(); }

    [[nodiscard]] Result<std::optional<T>> next() override {
        if (i_ >= items_.size()) {
            return std::optional<T>{};
        }
        return std::optional<T>{items_[i_++]};
    }

private:
    std::vector<T> items_;
    size_t i_{0};
};

using IntSource = VectorSource<int32_t>;
using StringSource = VectorSource<std::string>;

std::vector<std::unique_ptr<IteratorExt<int32_t>>> sources(const std::vector<std::vector<int32_t>> &items) {
    std::vector<std::unique_ptr<IteratorExt<int32_t>>> out;
    out.reserve(items.size());
    for (const std::vector<int32_t> &one : items) {
        out.push_back(std::make_unique<IntSource>(one));
    }
    return out;
}

std::vector<int32_t> drain(MergedIterator<int32_t> &merged) {
    std::vector<int32_t> out;
    while (true) {
        auto has = merged.has_next();
        EXPECT_TRUE(has.has_value());
        if (!has.has_value() || !has.value()) {
            break;
        }
        auto item = merged.next();
        EXPECT_TRUE(item.has_value()) << (item ? "" : item.error().to_string());
        if (!item.has_value()) {
            break;
        }
        out.push_back(item.value().value());
    }
    return out;
}

} // namespace

TEST(MergedIteratorTest, ReturnsTheSortedUnionByDefault) {
    auto merged = MergedIterator<int32_t>::create(
            sources({{1, 4, 7}, {2, 4, 8}, {3, 7, 9}, {}}));
    ASSERT_TRUE(merged.has_value()) << (merged ? "" : merged.error().to_string());
    EXPECT_EQ(drain(*merged.value()), (std::vector<int32_t>{1, 2, 3, 4, 7, 8, 9}));
}

TEST(MergedIteratorTest, KeepsDuplicatesWhenAskedTo) {
    auto merged = MergedIterator<int32_t>::create_with_remove_duplicates(
            false, sources({{1, 4, 7}, {2, 4, 8}, {3, 7, 9}}));
    ASSERT_TRUE(merged.has_value());
    auto items = drain(*merged.value());
    EXPECT_EQ(items.size(), 9U);
    // The order among duplicates is unspecified, but every source stays sorted and
    // the whole stream is non-decreasing.
    for (size_t i = 1; i < items.size(); ++i) {
        EXPECT_LE(items[i - 1], items[i]);
    }
    EXPECT_EQ(items.front(), 1);
    EXPECT_EQ(items.back(), 9);
}

TEST(MergedIteratorTest, ReportsExhaustion) {
    auto merged = MergedIterator<int32_t>::create(sources({{}, {}}));
    ASSERT_TRUE(merged.has_value());
    EXPECT_FALSE(merged.value()->has_next().value());
    auto item = merged.value()->next();
    ASSERT_FALSE(item.has_value());
    EXPECT_EQ(item.error().kind(), ErrorKind::kNoSuchElement);
}

TEST(MergedIteratorTest, PassesASingleSourceThrough) {
    auto merged = MergedIterator<int32_t>::create(sources({{5, 6, 6, 9}}));
    ASSERT_TRUE(merged.has_value());
    // Duplicates inside one iterator are not removed, matching the reference.
    EXPECT_EQ(drain(*merged.value()), (std::vector<int32_t>{5, 6, 6, 9}));
}

TEST(MergedIteratorTest, MergesStringsAsWell) {
    std::vector<std::unique_ptr<IteratorExt<std::string>>> inputs;
    inputs.push_back(std::make_unique<StringSource>(std::vector<std::string>{"a", "c"}));
    inputs.push_back(std::make_unique<StringSource>(std::vector<std::string>{"b", "c"}));
    auto merged = MergedIterator<std::string>::create(std::move(inputs));
    ASSERT_TRUE(merged.has_value());
    std::vector<std::string> out;
    while (merged.value()->has_next().value()) {
        out.push_back(merged.value()->next().value().value());
    }
    EXPECT_EQ(out, (std::vector<std::string>{"a", "b", "c"}));
}
