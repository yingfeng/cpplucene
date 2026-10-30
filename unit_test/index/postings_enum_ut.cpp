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

// gtest does not pull in these headers, and the library does not re-export
// `std`, so the declarations used by the fakes below live in the global module
// fragment.
#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <variant>
#include <vector>

module lucene_core:ut.postings_enum;

import lucene_core;

using namespace lucene;

namespace {

const std::vector<uint8_t> g_payload{0xAB, 0xCD};

/// A postings enum over the three docs 0, 1, 2 with frequencies 1, 2, 3.
class FakePostingsEnum : public PostingsEnum {
public:
    [[nodiscard]] int32_t doc_id() const override { return doc_ >= 3 ? NO_MORE_DOCS : doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override {
        ++doc_;
        return doc_ >= 3 ? NO_MORE_DOCS : doc_;
    }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        while (doc_ < target) {
            ++doc_;
        }
        return doc_ >= 3 ? NO_MORE_DOCS : doc_;
    }
    [[nodiscard]] Result<int64_t> cost() const override { return int64_t{3}; }

    [[nodiscard]] Result<int32_t> freq() override { return doc_ + 1; }
    [[nodiscard]] Result<int32_t> next_position() override { return ++position_; }
    [[nodiscard]] Result<int32_t> start_offset() const override { return position_; }
    [[nodiscard]] Result<int32_t> end_offset() const override { return position_ + 5; }
    [[nodiscard]] Result<std::optional<std::span<const uint8_t>>> get_payload() const override {
        return std::optional<std::span<const uint8_t>>(std::span<const uint8_t>(g_payload));
    }

private:
    int32_t doc_{-1};
    int32_t position_{-1};
};

/// A distinct type with the same behaviour, so that `std::variant` can hold two
/// different alternatives (the reference implementation's `PostingsEnumEnum2::A` / `::B`).
class OtherPostingsEnum : public FakePostingsEnum {};

/// Impacts over a single level.
class FakeImpacts : public Impacts {
public:
    [[nodiscard]] size_t num_levels() const override { return 1; }
    [[nodiscard]] int32_t get_doc_id_upto(size_t /*level*/) const override { return 2; }
    [[nodiscard]] Result<std::span<const Impact>> get_impacts(size_t /*level*/) const override {
        return std::span<const Impact>(impacts_);
    }

private:
    std::array<Impact, 2> impacts_{Impact{1, 4}, Impact{2, 8}};
};

/// `ImpactsEnum` over `FakePostingsEnum`, recording the last shallow target.
class FakeImpactsEnum : public ImpactsEnum {
public:
    [[nodiscard]] int32_t doc_id() const override { return postings_.doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override { return postings_.next_doc(); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override { return postings_.advance(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return postings_.cost(); }
    [[nodiscard]] Result<int32_t> freq() override { return postings_.freq(); }
    [[nodiscard]] Result<int32_t> next_position() override { return postings_.next_position(); }
    [[nodiscard]] Result<int32_t> start_offset() const override { return postings_.start_offset(); }
    [[nodiscard]] Result<int32_t> end_offset() const override { return postings_.end_offset(); }
    [[nodiscard]] Result<std::optional<std::span<const uint8_t>>> get_payload() const override {
        return postings_.get_payload();
    }
    [[nodiscard]] VoidResult advance_shallow(int32_t target) override {
        shallow_target_ = target;
        return {};
    }
    [[nodiscard]] Result<Impacts *> get_impacts() override { return &impacts_; }

    [[nodiscard]] int32_t shallow_target() const { return shallow_target_; }

private:
    FakePostingsEnum postings_;
    int32_t shallow_target_{-1};
    FakeImpacts impacts_;
};

/// Same behaviour, distinct type: the second variant alternative.
class OtherImpactsEnum : public FakeImpactsEnum {};

} // namespace

TEST(PostingsEnumTest, FeatureFlags) {
    EXPECT_EQ(NONE, 0);
    EXPECT_TRUE(feature_requested(FREQS, FREQS));
    EXPECT_FALSE(feature_requested(NONE, FREQS));
    // POSITIONS implies FREQS, OFFSETS implies POSITIONS, PAYLOADS implies
    // POSITIONS but not OFFSETS.
    EXPECT_TRUE(feature_requested(POSITIONS, FREQS));
    EXPECT_TRUE(feature_requested(OFFSETS, POSITIONS));
    EXPECT_TRUE(feature_requested(PAYLOADS, POSITIONS));
    EXPECT_FALSE(feature_requested(PAYLOADS, OFFSETS));
    EXPECT_TRUE(feature_requested(ALL, OFFSETS));
    EXPECT_TRUE(feature_requested(ALL, PAYLOADS));
    // `fetch_freqs`/`fetch_positions`/... of the reference implementation are derived from the same
    // word, so FREQS must not be reported when only positions were asked for.
    EXPECT_FALSE(feature_requested(POSITIONS, OFFSETS));
}

TEST(PostingsEnumTest, ConcreteEnumExposesPerDocMethods) {
    FakePostingsEnum postings;
    EXPECT_EQ(postings.doc_id(), -1);

    EXPECT_EQ(postings.next_doc().value(), 0);
    EXPECT_EQ(postings.doc_id(), 0);
    EXPECT_EQ(postings.freq().value(), 1);
    EXPECT_EQ(postings.next_position().value(), 0);
    EXPECT_EQ(postings.start_offset().value(), 0);
    EXPECT_EQ(postings.end_offset().value(), 5);

    EXPECT_EQ(postings.next_doc().value(), 1);
    EXPECT_EQ(postings.freq().value(), 2);

    auto payload = postings.get_payload();
    ASSERT_TRUE(payload.has_value());
    ASSERT_TRUE(payload.value().has_value());
    EXPECT_TRUE(std::ranges::equal(payload.value().value(), g_payload));

    EXPECT_EQ(postings.advance(2).value(), 2);
    EXPECT_EQ(postings.next_doc().value(), NO_MORE_DOCS);
    EXPECT_EQ(postings.doc_id(), NO_MORE_DOCS);
    EXPECT_EQ(postings.cost().value(), 3);
}

TEST(PostingsEnumTest, VariantEnumDispatchesToConcrete) {
    using Enum = PostingsEnumEnum<FakePostingsEnum, OtherPostingsEnum>;

    Enum first{std::variant<FakePostingsEnum, OtherPostingsEnum>{std::in_place_index<0>, FakePostingsEnum{}}};
    EXPECT_EQ(first.doc_id(), -1);
    EXPECT_EQ(first.next_doc().value(), 0);
    EXPECT_EQ(first.freq().value(), 1);
    EXPECT_EQ(first.next_doc().value(), 1);
    EXPECT_EQ(first.freq().value(), 2);
    EXPECT_EQ(first.cost().value(), 3);

    Enum second{std::variant<FakePostingsEnum, OtherPostingsEnum>{std::in_place_index<1>, OtherPostingsEnum{}}};
    EXPECT_EQ(second.advance(1).value(), 1);
    EXPECT_EQ(second.freq().value(), 2);
    EXPECT_EQ(second.next_doc().value(), 2);
}

TEST(ImpactsEnumTest, VariantForwardsImpactsAndPostings) {
    static_assert(std::is_base_of_v<PostingsEnum, ImpactsEnum>);
    static_assert(std::is_base_of_v<ImpactsSource, ImpactsEnum>);

    using Enum = ImpactsEnumEnum<FakeImpactsEnum, OtherImpactsEnum>;
    Enum enumeration{std::variant<FakeImpactsEnum, OtherImpactsEnum>{std::in_place_index<0>, FakeImpactsEnum{}}};

    EXPECT_EQ(enumeration.next_doc().value(), 0);
    EXPECT_EQ(enumeration.freq().value(), 1);
    ASSERT_TRUE(enumeration.advance_shallow(1).has_value());

    auto impacts = enumeration.get_impacts();
    ASSERT_TRUE(impacts.has_value());
    ASSERT_NE(impacts.value(), nullptr);
    EXPECT_EQ(impacts.value()->num_levels(), 1u);
    EXPECT_EQ(impacts.value()->get_doc_id_upto(0), 2);

    auto values = impacts.value()->get_impacts(0);
    ASSERT_TRUE(values.has_value());
    ASSERT_EQ(values.value().size(), 2u);
    EXPECT_EQ(values.value()[0], (Impact{1, 4}));
    EXPECT_EQ(values.value()[1], (Impact{2, 8}));

    auto payload = enumeration.get_payload();
    ASSERT_TRUE(payload.has_value());
    ASSERT_TRUE(payload.value().has_value());
    EXPECT_TRUE(std::ranges::equal(payload.value().value(), g_payload));
}

TEST(ImpactsEnumTest, ConcreteEnumRecordsShallowTarget) {
    FakeImpactsEnum enumeration;
    ASSERT_TRUE(enumeration.advance_shallow(2).has_value());
    EXPECT_EQ(enumeration.shallow_target(), 2);
    EXPECT_EQ(enumeration.next_doc().value(), 0);
}
