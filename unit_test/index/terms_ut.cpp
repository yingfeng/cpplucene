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
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.terms;

import lucene_core;

using namespace lucene;

namespace {

int32_t compare_bytes(std::span<const uint8_t> left, std::span<const uint8_t> right) {
    const size_t shared = std::min(left.size(), right.size());
    for (size_t i = 0; i < shared; ++i) {
        if (left[i] != right[i]) {
            return left[i] < right[i] ? -1 : 1;
        }
    }
    if (left.size() == right.size()) {
        return 0;
    }
    return left.size() < right.size() ? -1 : 1;
}

/// A [`TermsEnum`] over a fixed sorted term list.  `ord()` and
/// `seek_exact_with_ord` are unsupported on purpose, so `Terms::get_max` has to
/// take its binary search path.
class VectorTermsEnum : public TermsEnum {
public:
    explicit VectorTermsEnum(std::vector<BytesRef> terms) : terms_(std::move(terms)) {}

    Result<const BytesRef *> next() override {
        if (next_index_ >= terms_.size()) {
            return static_cast<const BytesRef *>(nullptr);
        }
        index_ = next_index_;
        ++next_index_;
        positioned_ = true;
        return &terms_[index_];
    }

    Result<AttributeSource *> attributes() override {
        return err(LuceneError::unsupported_operation("VectorTermsEnum has no attributes"));
    }

    Result<AttributeSource *> attributes_mut() override {
        return err(LuceneError::unsupported_operation("VectorTermsEnum has no attributes"));
    }

    Result<bool> seek_exact(std::span<const uint8_t> term) override {
        auto status = seek_ceil(term);
        if (!status) {
            return err(std::move(status.error()));
        }
        return status.value() == SeekStatus::Found;
    }

    Result<bool> prepare_seek_exact(std::span<const uint8_t> /*term*/) override { return true; }

    Result<bool> get_prepare_seek_exact_status(std::span<const uint8_t> target) override {
        return seek_exact(target);
    }

    Result<SeekStatus> seek_ceil(std::span<const uint8_t> term) override {
        auto found = std::lower_bound(
                terms_.begin(), terms_.end(), term,
                [](const BytesRef &ref, std::span<const uint8_t> target) {
                    return compare_bytes(ref.as_slice(), target) < 0;
                });
        if (found == terms_.end()) {
            positioned_ = false;
            next_index_ = terms_.size();
            return SeekStatus::End;
        }
        index_ = static_cast<size_t>(found - terms_.begin());
        next_index_ = index_ + 1;
        positioned_ = true;
        return compare_bytes(found->as_slice(), term) == 0 ? SeekStatus::Found : SeekStatus::NotFound;
    }

    VoidResult seek_exact_with_ord(int64_t /*ord*/) override {
        return err(LuceneError::unsupported_operation("VectorTermsEnum has no ordinals"));
    }

    VoidResult seek_exact_with_state(std::span<const uint8_t> /*term*/, const TermStateEnum & /*state*/) override {
        return err(LuceneError::unsupported_operation("VectorTermsEnum has no term state"));
    }

    Result<const BytesRef *> term() const override {
        if (!positioned_) {
            return err(LuceneError::illegal_state("VectorTermsEnum is not positioned"));
        }
        return &terms_[index_];
    }

    Result<int64_t> ord() const override {
        return err(LuceneError::unsupported_operation("VectorTermsEnum has no ordinals"));
    }

    Result<int32_t> doc_freq() override { return 1; }

    Result<int64_t> total_term_freq() override { return 1; }

    Result<std::unique_ptr<PostingsEnum>> postings_with_flags(std::unique_ptr<PostingsEnum> /*reuse*/,
                                                              int32_t /*flags*/) override {
        return err(LuceneError::not_implemented("VectorTermsEnum has no postings"));
    }

    Result<std::unique_ptr<ImpactsEnum>> impacts(int32_t /*flags*/) override {
        return err(LuceneError::not_implemented("VectorTermsEnum has no impacts"));
    }

    Result<TermStateEnum> term_state() override {
        return err(LuceneError::unsupported_operation("VectorTermsEnum has no term state"));
    }

private:
    std::vector<BytesRef> terms_;
    size_t index_{0};
    size_t next_index_{0};
    bool positioned_{false};
};

class VectorTerms : public Terms {
public:
    explicit VectorTerms(std::vector<std::string> terms) {
        for (const std::string &term : terms) {
            terms_.push_back(BytesRef::from_string(term));
        }
    }

    Result<std::unique_ptr<TermsEnum>> iterator() const override {
        return std::unique_ptr<TermsEnum>(std::make_unique<VectorTermsEnum>(terms_));
    }

    Result<int64_t> size() const override { return static_cast<int64_t>(terms_.size()); }
    Result<int64_t> sum_total_term_freq() const override { return static_cast<int64_t>(terms_.size()); }
    Result<int64_t> sum_doc_freq() const override { return static_cast<int64_t>(terms_.size()); }
    Result<int32_t> doc_count() const override { return static_cast<int32_t>(terms_.size()); }
    bool has_freqs() const override { return true; }
    bool has_offsets() const override { return false; }
    bool has_positions() const override { return false; }
    bool has_payloads() const override { return false; }

private:
    std::vector<BytesRef> terms_;
};

} // namespace

TEST(EmptyTermsTest, HasNoTermsAndNoStatistics) {
    EmptyTerms terms;
    auto iterator = terms.iterator();
    ASSERT_TRUE(iterator.has_value());
    auto term = iterator.value()->next();
    ASSERT_TRUE(term.has_value());
    EXPECT_EQ(term.value(), nullptr);

    EXPECT_EQ(terms.size().value(), 0);
    EXPECT_EQ(terms.sum_total_term_freq().value(), 0);
    EXPECT_EQ(terms.sum_doc_freq().value(), 0);
    EXPECT_EQ(terms.doc_count().value(), 0);
    EXPECT_FALSE(terms.has_freqs());
    EXPECT_FALSE(terms.has_offsets());
    EXPECT_FALSE(terms.has_positions());
    EXPECT_FALSE(terms.has_payloads());

    EXPECT_FALSE(terms.get_min().value().has_value());
    EXPECT_FALSE(terms.get_max().value().has_value());
}

TEST(TermsTest, MinAndMaxBinarySearchWithoutOrdinals) {
    VectorTerms terms({"a", "ab", "b"});
    auto min = terms.get_min();
    ASSERT_TRUE(min.has_value());
    ASSERT_TRUE(min.value().has_value());
    EXPECT_EQ(min.value().value(), BytesRef::from_string("a"));

    auto max = terms.get_max();
    ASSERT_TRUE(max.has_value());
    ASSERT_TRUE(max.value().has_value());
    EXPECT_EQ(max.value().value(), BytesRef::from_string("b"));
}

TEST(TermsTest, MinAndMaxOfASingleTerm) {
    VectorTerms terms({"zz"});
    auto min = terms.get_min();
    ASSERT_TRUE(min.has_value());
    ASSERT_TRUE(min.value().has_value());
    EXPECT_EQ(min.value().value(), BytesRef::from_string("zz"));

    auto max = terms.get_max();
    ASSERT_TRUE(max.has_value());
    ASSERT_TRUE(max.value().has_value());
    EXPECT_EQ(max.value().value(), BytesRef::from_string("zz"));
}

TEST(TermsTest, StatsReportTheTermCounts) {
    VectorTerms terms({"a", "b"});
    auto stats = terms.get_stats();
    ASSERT_TRUE(stats.has_value());
    EXPECT_NE(stats.value().find("size=2"), std::string::npos);
    EXPECT_NE(stats.value().find("docCount=2"), std::string::npos);
    EXPECT_NE(stats.value().find("sumTotalTermFreq=2"), std::string::npos);
    EXPECT_NE(stats.value().find("sumDocFreq=2"), std::string::npos);
}
