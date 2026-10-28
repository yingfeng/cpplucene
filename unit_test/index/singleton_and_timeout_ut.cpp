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

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <thread>
#include <unordered_map>

module lucene_core:ut.singleton_and_timeout;

import lucene_core;

using namespace lucene;

namespace {

/// A [`SortedDocValues`] over an in-memory `doc -> ord` map.
class MapSortedDocValues final : public SortedDocValues {
public:
    MapSortedDocValues(std::unordered_map<int32_t, int32_t> ords, std::vector<std::string> dictionary)
        : ords_(std::move(ords)), dictionary_(std::move(dictionary)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override { return advance(doc_ + 1); }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        int32_t candidate = target;
        while (candidate <= max_doc_ && !ords_.contains(candidate)) {
            candidate += 1;
        }
        doc_ = candidate > max_doc_ ? NO_MORE_DOCS : candidate;
        return doc_;
    }

    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        doc_ = target;
        return ords_.contains(target);
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(ords_.size()); }

    [[nodiscard]] Result<int32_t> ord_value() override {
        auto it = ords_.find(doc_);
        return it == ords_.end() ? -1 : it->second;
    }

    [[nodiscard]] Result<BytesRef> lookup_ord(int32_t ord) override {
        if (ord < 0 || static_cast<size_t>(ord) >= dictionary_.size()) {
            return err(LuceneError::illegal_argument("ord out of range"));
        }
        return BytesRef::from_string(dictionary_[static_cast<size_t>(ord)]);
    }

    [[nodiscard]] Result<int32_t> get_value_count() const override {
        return static_cast<int32_t>(dictionary_.size());
    }

    void set_max_doc(int32_t max_doc) noexcept { max_doc_ = max_doc; }

private:
    std::unordered_map<int32_t, int32_t> ords_;
    std::vector<std::string> dictionary_;
    int32_t doc_{-1};
    int32_t max_doc_{4};
};

std::unique_ptr<SortedDocValues> make_sorted() {
    return std::make_unique<MapSortedDocValues>(std::unordered_map<int32_t, int32_t>{{0, 0}, {2, 1}, {4, 2}},
                                                std::vector<std::string>{"a", "b", "c"});
}

} // namespace

TEST(SingletonSortedSetDocValuesTest, PresentsOneOrdinalPerDocument) {
    SingletonSortedSetDocValues values(make_sorted());
    EXPECT_TRUE(values.is_single_valued());

    EXPECT_EQ(values.doc_id(), -1);
    EXPECT_EQ(values.next_doc().value(), 0);
    EXPECT_EQ(values.next_ord().value(), 0);
    // The reference repeats the ordinal within the same document.
    EXPECT_EQ(values.next_ord().value(), 0);
    EXPECT_EQ(values.doc_value_count().value(), 1);

    EXPECT_EQ(values.next_doc().value(), 2);
    EXPECT_EQ(values.next_ord().value(), 1);

    // A document without a value reports false, and the cached ordinal is left
    // untouched - the reference implementation behaves the same way.
    EXPECT_FALSE(values.advance_exact(3).value());
    EXPECT_EQ(values.next_ord().value(), 1);

    ASSERT_TRUE(values.advance_exact(4).value());
    EXPECT_EQ(values.next_ord().value(), 2);
    EXPECT_EQ(values.next_doc().value(), NO_MORE_DOCS);
}

TEST(SingletonSortedSetDocValuesTest, ForwardsDictionaryLookups) {
    SingletonSortedSetDocValues values(make_sorted());
    EXPECT_EQ(values.get_value_count().value(), 3);
    EXPECT_EQ(values.lookup_ord(1).value().utf8_to_string().value(), "b");
    EXPECT_FALSE(values.lookup_ord(-1).has_value());
    EXPECT_FALSE(values.lookup_ord(3).has_value());
    EXPECT_EQ(values.cost().value(), 3);

    // `lookup_term` is inherited from SortedSetDocValues' binary search.
    EXPECT_EQ(values.lookup_term(std::span<const uint8_t>(
                      reinterpret_cast<const uint8_t *>("b"), 1))
                      .value(),
              1);
}

TEST(SingletonSortedSetDocValuesTest, AdvanceExactReportsMissingValues) {
    SingletonSortedSetDocValues values(make_sorted());
    EXPECT_FALSE(values.advance_exact(1).value());
    EXPECT_EQ(values.doc_id(), 1);
    EXPECT_TRUE(values.advance_exact(2).value());
    EXPECT_EQ(values.next_ord().value(), 1);
}

TEST(QueryTimeoutImplTest, TimesOutAfterTheDeadline) {
    QueryTimeoutImpl immediate(0);
    EXPECT_TRUE(immediate.should_exit());
    EXPECT_TRUE(immediate.timeout_at_nanos().has_value());
    EXPECT_NE(immediate.to_string().find("timeoutAt"), std::string::npos);

    QueryTimeoutImpl never(-1);
    EXPECT_FALSE(never.should_exit());
    EXPECT_FALSE(never.timeout_at_nanos().has_value());
    EXPECT_EQ(never.to_string(), "QueryTimeoutImpl(disabled)");

    QueryTimeoutImpl also_never(std::numeric_limits<int64_t>::max());
    EXPECT_FALSE(also_never.should_exit());

    // A generous deadline does not trip, a tiny one eventually does.
    QueryTimeoutImpl generous(60'000);
    EXPECT_FALSE(generous.should_exit());
    QueryTimeoutImpl short_timeout(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    EXPECT_TRUE(short_timeout.should_exit());
}

TEST(QueryTimeoutImplTest, ResetDisablesTheTimeout) {
    QueryTimeoutImpl timeout(0);
    ASSERT_TRUE(timeout.should_exit());
    timeout.reset();
    EXPECT_FALSE(timeout.timeout_at_nanos().has_value());
    EXPECT_FALSE(timeout.should_exit());
}
