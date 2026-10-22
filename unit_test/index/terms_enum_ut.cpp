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
#include <span>

module lucene_core:ut.terms_enum;

import lucene_core;

using namespace lucene;

namespace {

/// Records the flags of the last `postings_with_flags` call.
class RecordingTermsEnum : public TermsEnum {
public:
    [[nodiscard]] Result<AttributeSource *> attributes() override { return &attributes_; }
    [[nodiscard]] Result<AttributeSource *> attributes_mut() override { return &attributes_; }

    [[nodiscard]] Result<bool> seek_exact(std::span<const uint8_t> term) override {
        return term.empty() && term_.as_slice().empty();
    }
    [[nodiscard]] Result<bool> prepare_seek_exact(std::span<const uint8_t> /*term*/) override { return false; }
    [[nodiscard]] Result<bool> get_prepare_seek_exact_status(std::span<const uint8_t> target) override {
        return seek_exact(target);
    }
    [[nodiscard]] Result<SeekStatus> seek_ceil(std::span<const uint8_t> /*term*/) override {
        return SeekStatus::NotFound;
    }
    [[nodiscard]] VoidResult seek_exact_with_ord(int64_t /*ord*/) override { return {}; }
    [[nodiscard]] VoidResult seek_exact_with_state(std::span<const uint8_t> /*term*/,
                                                   const TermStateEnum & /*state*/) override {
        return {};
    }

    [[nodiscard]] Result<const BytesRef *> term() const override { return &term_; }
    [[nodiscard]] Result<int64_t> ord() const override { return int64_t{7}; }
    [[nodiscard]] Result<int32_t> doc_freq() override { return int32_t{2}; }
    [[nodiscard]] Result<int64_t> total_term_freq() override { return int64_t{5}; }

    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>>
    postings_with_flags(std::unique_ptr<PostingsEnum> /*reuse*/, int32_t flags) override {
        last_flags = flags;
        return std::unique_ptr<PostingsEnum>(nullptr);
    }

    [[nodiscard]] Result<std::unique_ptr<ImpactsEnum>> impacts(int32_t /*flags*/) override {
        return std::unique_ptr<ImpactsEnum>(nullptr);
    }

    [[nodiscard]] Result<TermStateEnum> term_state() override { return TermStateEnum{BlockTermState{}}; }

    int32_t last_flags{-1};

private:
    AttributeSource attributes_;
    BytesRef term_{BytesRef::from_string("abc")};
};

} // namespace

TEST(BytesRefIteratorTest, EmptyIteratorIsExhausted) {
    EmptyBytesRefIterator iterator;
    auto value = iterator.next();
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value.value(), nullptr);
    EXPECT_FALSE(iterator.set_next().has_value());
}

TEST(EmptyTermsEnumTest, IterationAndSeeksAreEmpty) {
    EmptyTermsEnum terms;
    auto next = terms.next();
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next.value(), nullptr);
    EXPECT_EQ(terms.seek_ceil(BytesRef::from_string("a").as_slice()).value(), SeekStatus::End);
    EXPECT_FALSE(terms.seek_exact(BytesRef::from_string("a").as_slice()).value());
    EXPECT_TRUE(terms.prepare_seek_exact(BytesRef::from_string("a").as_slice()).value());
    EXPECT_TRUE(terms.seek_exact_with_ord(3).has_value());
}

TEST(EmptyTermsEnumTest, UnsupportedOperationsReportErrors) {
    EmptyTermsEnum terms;

    auto attributes = terms.attributes();
    ASSERT_FALSE(attributes.has_value());
    EXPECT_EQ(attributes.error().kind(), ErrorKind::kUnsupportedOperation);

    auto ord = terms.ord();
    ASSERT_FALSE(ord.has_value());
    EXPECT_EQ(ord.error().kind(), ErrorKind::kIllegalState);

    auto doc_freq = terms.doc_freq();
    ASSERT_FALSE(doc_freq.has_value());
    EXPECT_EQ(doc_freq.error().kind(), ErrorKind::kIllegalState);

    auto postings = terms.postings(nullptr);
    ASSERT_FALSE(postings.has_value());
    EXPECT_EQ(postings.error().kind(), ErrorKind::kIllegalState);

    auto impacts = terms.impacts(FREQS);
    ASSERT_FALSE(impacts.has_value());
    EXPECT_EQ(impacts.error().kind(), ErrorKind::kIllegalState);

    auto state = terms.term_state();
    ASSERT_FALSE(state.has_value());
    EXPECT_EQ(state.error().kind(), ErrorKind::kIllegalState);
}

TEST(TermsEnumTest, PostingsDefaultsToFreqFlags) {
    RecordingTermsEnum terms;

    ASSERT_TRUE(terms.postings(nullptr).has_value());
    EXPECT_EQ(terms.last_flags, static_cast<int32_t>(FREQS));

    ASSERT_TRUE(terms.postings_with_flags(nullptr, ALL).has_value());
    EXPECT_EQ(terms.last_flags, static_cast<int32_t>(ALL));

    EXPECT_EQ(terms.doc_freq().value(), 2);
    EXPECT_EQ(terms.total_term_freq().value(), 5);
    EXPECT_EQ(terms.ord().value(), 7);
    auto term = terms.term();
    ASSERT_TRUE(term.has_value());
    EXPECT_EQ(term.value()->utf8_to_string(), "abc");
    EXPECT_NE(terms.attributes().value(), nullptr);
}
