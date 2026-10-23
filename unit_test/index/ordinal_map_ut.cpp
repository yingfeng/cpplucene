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

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.ordinal_map;

import lucene_core;

using namespace lucene;

namespace {

/// A term enum over a sorted list of dummy terms; only `next`, `term` and `ord`
/// are implemented, which is all an ordinal map needs.
class TermListEnum final : public TermsEnum {
public:
    explicit TermListEnum(int32_t term_count) {
        for (int32_t i = 0; i < term_count; ++i) {
            terms_.push_back(BytesRef::from_string("t" + std::to_string(i)));
        }
    }

    [[nodiscard]] Result<const BytesRef *> next() override {
        if (!position_.has_value()) {
            if (terms_.empty()) {
                return static_cast<const BytesRef *>(nullptr);
            }
            position_ = 0;
            return &terms_[0];
        }
        if (position_.value() + 1 >= terms_.size()) {
            position_.reset();
            return static_cast<const BytesRef *>(nullptr);
        }
        position_ = position_.value() + 1;
        return &terms_[position_.value()];
    }

    [[nodiscard]] Result<const BytesRef *> term() const override {
        if (!position_.has_value()) {
            return static_cast<const BytesRef *>(nullptr);
        }
        return &terms_[position_.value()];
    }

    [[nodiscard]] Result<int64_t> ord() const override {
        if (!position_.has_value()) {
            return err(LuceneError::illegal_state("no current term"));
        }
        return static_cast<int64_t>(position_.value());
    }

    [[nodiscard]] Result<int64_t> total_term_freq() override {
        return err(LuceneError::unsupported_operation("TermListEnum::total_term_freq"));
    }

    [[nodiscard]] Result<bool> seek_exact(std::span<const uint8_t> /*term*/) override {
        return err(LuceneError::unsupported_operation("TermListEnum::seek_exact"));
    }
    [[nodiscard]] Result<bool> prepare_seek_exact(std::span<const uint8_t> /*term*/) override {
        return err(LuceneError::unsupported_operation("TermListEnum::prepare_seek_exact"));
    }
    [[nodiscard]] Result<bool> get_prepare_seek_exact_status(std::span<const uint8_t> /*target*/) override {
        return err(LuceneError::unsupported_operation("TermListEnum::get_prepare_seek_exact_status"));
    }
    [[nodiscard]] Result<SeekStatus> seek_ceil(std::span<const uint8_t> /*term*/) override {
        return err(LuceneError::unsupported_operation("TermListEnum::seek_ceil"));
    }
    [[nodiscard]] VoidResult seek_exact_with_ord(int64_t /*ord*/) override {
        return err(LuceneError::unsupported_operation("TermListEnum::seek_exact_with_ord"));
    }
    [[nodiscard]] VoidResult seek_exact_with_state(std::span<const uint8_t> /*term*/,
                                                   const TermStateEnum & /*state*/) override {
        return err(LuceneError::unsupported_operation("TermListEnum::seek_exact_with_state"));
    }
    [[nodiscard]] Result<int32_t> doc_freq() override {
        return err(LuceneError::unsupported_operation("TermListEnum::doc_freq"));
    }
    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>> postings_with_flags(std::unique_ptr<PostingsEnum> /*reuse*/,
                                                                          int32_t /*flags*/) override {
        return err(LuceneError::unsupported_operation("TermListEnum::postings_with_flags"));
    }
    [[nodiscard]] Result<std::unique_ptr<ImpactsEnum>> impacts(int32_t /*flags*/) override {
        return err(LuceneError::unsupported_operation("TermListEnum::impacts"));
    }
    [[nodiscard]] Result<TermStateEnum> term_state() override {
        return err(LuceneError::unsupported_operation("TermListEnum::term_state"));
    }
    [[nodiscard]] Result<AttributeSource *> attributes() override {
        return err(LuceneError::unsupported_operation("TermListEnum::attributes"));
    }
    [[nodiscard]] Result<AttributeSource *> attributes_mut() override {
        return err(LuceneError::unsupported_operation("TermListEnum::attributes_mut"));
    }

private:
    std::vector<BytesRef> terms_;
    std::optional<size_t> position_;
};

} // namespace

TEST(SegmentMapTest, OrdersSegmentsByDescendingWeight) {
    auto map = SegmentMap::create(std::vector<int64_t>{5, 9, 1});
    ASSERT_TRUE(map.has_value()) << (map ? "" : map.error().to_string());
    EXPECT_EQ(map.value().size(), 3U);

    // Heaviest first: segment 1, then 0, then 2.
    EXPECT_EQ(map.value().new_to_old(0), 1U);
    EXPECT_EQ(map.value().new_to_old(1), 0U);
    EXPECT_EQ(map.value().new_to_old(2), 2U);
    EXPECT_EQ(map.value().old_to_new(0), 1U);
    EXPECT_EQ(map.value().old_to_new(1), 0U);
    EXPECT_EQ(map.value().old_to_new(2), 2U);
}

TEST(OrdinalMapTest, MergesTwoSegmentsIntoOneGlobalSpace) {
    // Segment 0 has two terms, segment 1 has three; both contain the middle term,
    // which the merge must collapse to a single global ordinal.
    std::vector<std::unique_ptr<TermsEnum>> subs;
    subs.push_back(std::make_unique<TermListEnum>(2));
    subs.push_back(std::make_unique<TermListEnum>(3));

    auto map = OrdinalMap::build(std::move(subs), std::vector<int64_t>{3, 2}, 0.25F);
    ASSERT_TRUE(map.has_value()) << (map ? "" : map.error().to_string());

    // Global terms: t0, t1, t2.
    EXPECT_EQ(map.value().value_count(), 3);

    // Segment 0 is the heavier one, so it leads the segment map.
    EXPECT_EQ(map.value().segment_map().new_to_old(0), 0U);

    // t0 exists in both segments, t1 only in segment 1, t2 only in segment 1.
    EXPECT_EQ(map.value().get_global_ord(0, 0).value(), 0);
    EXPECT_EQ(map.value().get_global_ord(0, 1).value(), 1);
    EXPECT_EQ(map.value().get_global_ord(1, 0).value(), 0);
    EXPECT_EQ(map.value().get_global_ord(1, 1).value(), 1);
    EXPECT_EQ(map.value().get_global_ord(1, 2).value(), 2);

    // The first segment that holds a term is reported with its original index.
    EXPECT_EQ(map.value().get_first_segment_number(0).value(), 0U);
    EXPECT_EQ(map.value().get_first_segment_number(1).value(), 0U);
    EXPECT_EQ(map.value().get_first_segment_number(2).value(), 1U);

    // ... and with the ordinal it has there.
    EXPECT_EQ(map.value().get_first_segment_ord(0).value(), 0);
    EXPECT_EQ(map.value().get_first_segment_ord(1).value(), 1);
    EXPECT_EQ(map.value().get_first_segment_ord(2).value(), 2);
}

TEST(OrdinalMapTest, UsesIdentityWhenOrdinalsAlreadyMatch) {
    // A single segment needs no remapping at all.
    std::vector<std::unique_ptr<TermsEnum>> subs;
    subs.push_back(std::make_unique<TermListEnum>(4));

    auto map = OrdinalMap::build(std::move(subs), std::vector<int64_t>{4}, 0.25F);
    ASSERT_TRUE(map.has_value()) << (map ? "" : map.error().to_string());
    EXPECT_EQ(map.value().value_count(), 4);
    for (int64_t ord = 0; ord < 4; ++ord) {
        EXPECT_EQ(map.value().get_global_ord(0, ord).value(), ord);
        EXPECT_EQ(map.value().get_first_segment_ord(ord).value(), ord);
        EXPECT_EQ(map.value().get_first_segment_number(ord).value(), 0U);
    }
    EXPECT_TRUE(map.value().segment_to_global_ords(0).is_identity());

    // Two segments with identical term sets keep the identity mapping as well.
    std::vector<std::unique_ptr<TermsEnum>> identical;
    identical.push_back(std::make_unique<TermListEnum>(3));
    identical.push_back(std::make_unique<TermListEnum>(3));
    auto duplicated = OrdinalMap::build(std::move(identical), std::vector<int64_t>{3, 3}, 0.25F);
    ASSERT_TRUE(duplicated.has_value());
    EXPECT_EQ(duplicated.value().value_count(), 3);
    EXPECT_EQ(duplicated.value().get_global_ord(1, 2).value(), 2);
    EXPECT_TRUE(duplicated.value().segment_to_global_ords(1).is_identity());
}

TEST(OrdinalMapTest, HandlesDisjointAndEmptySegments) {
    // The second segment only holds the later terms, so its ordinals shift.
    std::vector<std::unique_ptr<TermsEnum>> subs;
    subs.push_back(std::make_unique<TermListEnum>(1));
    subs.push_back(std::make_unique<TermListEnum>(3));
    auto map = OrdinalMap::build(std::move(subs), std::vector<int64_t>{1, 3}, 0.25F);
    ASSERT_TRUE(map.has_value()) << (map ? "" : map.error().to_string());
    EXPECT_EQ(map.value().value_count(), 3);
    // Segment 1's terms are t0, t1, t2 -> global 0, 1, 2; segment 0 only has t0.
    EXPECT_EQ(map.value().get_global_ord(0, 0).value(), 0);
    EXPECT_EQ(map.value().get_global_ord(1, 2).value(), 2);

    // An empty segment contributes nothing but is still mapped.
    std::vector<std::unique_ptr<TermsEnum>> with_empty;
    with_empty.push_back(std::make_unique<TermListEnum>(0));
    with_empty.push_back(std::make_unique<TermListEnum>(2));
    auto empty_map = OrdinalMap::build(std::move(with_empty), std::vector<int64_t>{0, 2}, 0.25F);
    ASSERT_TRUE(empty_map.has_value()) << (empty_map ? "" : empty_map.error().to_string());
    EXPECT_EQ(empty_map.value().value_count(), 2);
    EXPECT_EQ(empty_map.value().get_global_ord(1, 1).value(), 1);
}

TEST(OrdinalMapTest, RejectsInvalidInputs) {
    std::vector<std::unique_ptr<TermsEnum>> mismatched;
    mismatched.push_back(std::make_unique<TermListEnum>(1));
    auto bad = OrdinalMap::build(std::move(mismatched), std::vector<int64_t>{1, 2}, 0.25F);
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().kind(), ErrorKind::kIllegalArgument);

    auto none = OrdinalMap::build({}, std::span<const int64_t>{}, 0.25F);
    ASSERT_FALSE(none.has_value());
    EXPECT_EQ(none.error().kind(), ErrorKind::kIllegalArgument);

    // Out of range ordinals are rejected by the accessors.
    std::vector<std::unique_ptr<TermsEnum>> subs;
    subs.push_back(std::make_unique<TermListEnum>(2));
    auto map = OrdinalMap::build(std::move(subs), std::vector<int64_t>{2}, 0.25F);
    ASSERT_TRUE(map.has_value());
    EXPECT_FALSE(map.value().get_first_segment_ord(5).has_value());
    EXPECT_FALSE(map.value().get_first_segment_number(5).has_value());
    EXPECT_FALSE(map.value().segment_to_global_ords(0).get(-1).has_value());
}
