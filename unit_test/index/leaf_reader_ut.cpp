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
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

module lucene_core:ut.leaf_reader;

import lucene_core;

using namespace lucene;

namespace {

/// A [`TermsEnum`] over a sorted vector of terms with per-term statistics.
class MapTermsEnum final : public TermsEnum {
public:
    MapTermsEnum(std::vector<BytesRef> terms, std::vector<int32_t> doc_freqs, std::vector<int64_t> total_term_freqs)
        : terms_(std::move(terms)), doc_freqs_(std::move(doc_freqs)), total_term_freqs_(std::move(total_term_freqs)) {}

    [[nodiscard]] Result<const BytesRef *> next() override {
        if (!pos_.has_value()) {
            if (terms_.empty()) {
                return static_cast<const BytesRef *>(nullptr);
            }
            pos_ = 0;
            return &terms_[0];
        }
        if (pos_.value() + 1 >= terms_.size()) {
            pos_.reset();
            return static_cast<const BytesRef *>(nullptr);
        }
        pos_ = pos_.value() + 1;
        return &terms_[pos_.value()];
    }

    [[nodiscard]] Result<AttributeSource *> attributes() override {
        return err(LuceneError::unsupported_operation("MapTermsEnum has no attributes"));
    }
    [[nodiscard]] Result<AttributeSource *> attributes_mut() override {
        return err(LuceneError::unsupported_operation("MapTermsEnum has no attributes"));
    }

    [[nodiscard]] Result<SeekStatus> seek_ceil(std::span<const uint8_t> term) override {
        pos_.reset();
        for (size_t i = 0; i < terms_.size(); ++i) {
            const auto candidate = terms_[i].as_slice();
            if (std::ranges::lexicographical_compare(candidate, term)) {
                continue;
            }
            pos_ = i;
            return std::ranges::equal(candidate, term) ? SeekStatus::Found : SeekStatus::NotFound;
        }
        return SeekStatus::End;
    }

    [[nodiscard]] Result<bool> seek_exact(std::span<const uint8_t> term) override {
        auto status = seek_ceil(term);
        if (!status.has_value()) {
            return std::unexpected(std::move(status.error()));
        }
        return status.value() == SeekStatus::Found;
    }

    [[nodiscard]] Result<bool> prepare_seek_exact(std::span<const uint8_t> term) override { return seek_exact(term); }

    [[nodiscard]] Result<bool> get_prepare_seek_exact_status(std::span<const uint8_t> term) override {
        return seek_exact(term);
    }

    [[nodiscard]] VoidResult seek_exact_with_ord(int64_t ord) override {
        if (ord < 0 || static_cast<size_t>(ord) >= terms_.size()) {
            return err(LuceneError::illegal_argument("ord out of range"));
        }
        pos_ = static_cast<size_t>(ord);
        return {};
    }

    [[nodiscard]] VoidResult seek_exact_with_state(std::span<const uint8_t> /*term*/,
                                                   const TermStateEnum & /*state*/) override {
        return err(LuceneError::unsupported_operation("MapTermsEnum has no term state"));
    }

    [[nodiscard]] Result<const BytesRef *> term() const override {
        if (!pos_.has_value()) {
            return err(LuceneError::illegal_state("unpositioned"));
        }
        return &terms_[pos_.value()];
    }

    [[nodiscard]] Result<int64_t> ord() const override { return static_cast<int64_t>(pos_.value_or(0)); }

    [[nodiscard]] Result<int32_t> doc_freq() override {
        if (!pos_.has_value()) {
            return err(LuceneError::illegal_state("unpositioned"));
        }
        return doc_freqs_[pos_.value()];
    }

    [[nodiscard]] Result<int64_t> total_term_freq() override {
        if (!pos_.has_value()) {
            return err(LuceneError::illegal_state("unpositioned"));
        }
        return total_term_freqs_[pos_.value()];
    }

    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>> postings_with_flags(std::unique_ptr<PostingsEnum> /*reuse*/,
                                                                          int32_t /*flags*/) override {
        return err(LuceneError::unsupported_operation("MapTermsEnum has no postings"));
    }

    [[nodiscard]] Result<std::unique_ptr<ImpactsEnum>> impacts(int32_t /*flags*/) override {
        return err(LuceneError::unsupported_operation("MapTermsEnum has no impacts"));
    }

    [[nodiscard]] Result<TermStateEnum> term_state() override {
        return err(LuceneError::unsupported_operation("MapTermsEnum has no term state"));
    }

private:
    std::vector<BytesRef> terms_;
    std::vector<int32_t> doc_freqs_;
    std::vector<int64_t> total_term_freqs_;
    std::optional<size_t> pos_{};
};

/// [`Terms`] over a sorted vector of terms with per-term statistics.
class MapTerms final : public Terms {
public:
    MapTerms(std::vector<BytesRef> terms, std::vector<int32_t> doc_freqs, std::vector<int64_t> total_term_freqs)
        : terms_(std::move(terms)), doc_freqs_(std::move(doc_freqs)), total_term_freqs_(std::move(total_term_freqs)) {}

    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> iterator() const override {
        return std::unique_ptr<TermsEnum>(std::make_unique<MapTermsEnum>(terms_, doc_freqs_, total_term_freqs_));
    }

    [[nodiscard]] Result<int64_t> size() const override { return static_cast<int64_t>(terms_.size()); }

    [[nodiscard]] Result<int64_t> sum_total_term_freq() const override {
        int64_t sum = 0;
        for (const auto freq : total_term_freqs_) {
            sum += freq;
        }
        return sum;
    }

    [[nodiscard]] Result<int64_t> sum_doc_freq() const override {
        int64_t sum = 0;
        for (const auto freq : doc_freqs_) {
            sum += freq;
        }
        return sum;
    }

    [[nodiscard]] Result<int32_t> doc_count() const override { return 2; }

    [[nodiscard]] bool has_freqs() const override { return true; }
    [[nodiscard]] bool has_offsets() const override { return false; }
    [[nodiscard]] bool has_positions() const override { return false; }
    [[nodiscard]] bool has_payloads() const override { return false; }

private:
    std::vector<BytesRef> terms_;
    std::vector<int32_t> doc_freqs_;
    std::vector<int64_t> total_term_freqs_;
};

/// Minimal [`LeafReader`]: one field with two terms, no deletions.
class TestLeafReader final : public LeafReader {
public:
    TestLeafReader(int32_t max_doc, int32_t num_docs, bool with_terms)
        : max_doc_(max_doc), num_docs_(num_docs), with_terms_(with_terms) {}

    [[nodiscard]] Result<int32_t> max_doc() const override { return max_doc_; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return num_docs_; }

    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override { return nullptr; }
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override { return nullptr; }

    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view field) override {
        if (!with_terms_ || field != "body") {
            return std::unique_ptr<Terms>{};
        }
        return std::unique_ptr<Terms>(std::make_unique<MapTerms>(
                std::vector<BytesRef>{BytesRef::from_string("bar"), BytesRef::from_string("foo")},
                std::vector<int32_t>{2, 3}, std::vector<int64_t>{4, 7}));
    }

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric_doc_values(std::string_view /*field*/) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> get_binary_doc_values(std::string_view /*field*/) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted_doc_values(std::string_view /*field*/) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>>
    get_sorted_numeric_doc_values(std::string_view /*field*/) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<SortedSetDocValues>>
    get_sorted_set_doc_values(std::string_view /*field*/) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norm_values(std::string_view /*field*/) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<DocValuesSkipper>> get_doc_values_skipper(std::string_view /*field*/) override {
        return nullptr;
    }

    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return &field_infos_; }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return static_cast<const Bits *>(nullptr); }

private:
    int32_t max_doc_;
    int32_t num_docs_;
    bool with_terms_;
    FieldInfos field_infos_{FieldInfos::none()};
};

} // namespace

TEST(LeafReaderTest, SuperficialValuesComeFromTheImplementation) {
    TestLeafReader reader(/*max_doc=*/5, /*num_docs=*/4, /*with_terms=*/true);

    EXPECT_EQ(reader.max_doc().value(), 5);
    EXPECT_EQ(reader.num_docs().value(), 4);
    EXPECT_EQ(reader.num_deleted_docs().value(), 1);
    EXPECT_TRUE(reader.has_deletions().value());
    EXPECT_EQ(reader.to_string(), "LeafReader(max_doc=5 num_docs=4)");
}

TEST(LeafReaderTest, NoDeletionsIsReported) {
    TestLeafReader reader(/*max_doc=*/3, /*num_docs=*/3, /*with_terms=*/false);

    EXPECT_EQ(reader.num_deleted_docs().value(), 0);
    EXPECT_FALSE(reader.has_deletions().value());
    EXPECT_EQ(reader.live_docs().value(), nullptr);
}

TEST(LeafReaderTest, DocFreqSeeksTheTerms) {
    TestLeafReader reader(/*max_doc=*/4, /*num_docs=*/4, /*with_terms=*/true);

    EXPECT_EQ(reader.doc_freq(Term::from_text("body", "foo")).value(), 3);
    EXPECT_EQ(reader.doc_freq(Term::from_text("body", "bar")).value(), 2);
    // A missing term, a missing field.
    EXPECT_EQ(reader.doc_freq(Term::from_text("body", "zoo")).value(), 0);
    EXPECT_EQ(reader.doc_freq(Term::from_text("title", "foo")).value(), 0);
}

TEST(LeafReaderTest, TotalTermFreqComesFromTheTermsEnum) {
    TestLeafReader reader(/*max_doc=*/4, /*num_docs=*/4, /*with_terms=*/true);

    EXPECT_EQ(reader.total_term_freq(Term::from_text("body", "foo")).value(), 7);
    EXPECT_EQ(reader.total_term_freq(Term::from_text("body", "zoo")).value(), 0);
    EXPECT_EQ(reader.total_term_freq(Term::from_text("title", "foo")).value(), 0);
}

TEST(LeafReaderTest, FieldStatisticsFallBackToTerms) {
    TestLeafReader reader(/*max_doc=*/4, /*num_docs=*/4, /*with_terms=*/true);

    EXPECT_EQ(reader.sum_doc_freq("body").value(), 5);
    EXPECT_EQ(reader.sum_total_term_freq("body").value(), 11);
    EXPECT_EQ(reader.doc_count("body").value(), 2);
    // A field without terms reports zeroes.
    EXPECT_EQ(reader.sum_doc_freq("title").value(), 0);
    EXPECT_EQ(reader.sum_total_term_freq("title").value(), 0);
    EXPECT_EQ(reader.doc_count("title").value(), 0);
}

TEST(LeafReaderTest, LeavesIsItself) {
    TestLeafReader reader(/*max_doc=*/5, /*num_docs=*/4, /*with_terms=*/false);

    auto leaves = reader.leaves();
    ASSERT_TRUE(leaves.has_value());
    ASSERT_EQ(leaves.value().size(), 1U);

    const LeafReaderContext &leaf = leaves.value()[0];
    EXPECT_EQ(leaf.reader(), &reader);
    EXPECT_EQ(leaf.ord(), 0);
    EXPECT_EQ(leaf.doc_base(), 0);
    EXPECT_EQ(leaf.top_parent().leaves_num, 1);
    EXPECT_EQ(leaf.top_parent().max_doc, 5);
    EXPECT_TRUE(leaf.top_parent().id == ReaderUtil::get_top_level_context(leaf).id);
    // Repeated calls reuse the same array.
    EXPECT_EQ(reader.leaves().value().data(), leaves.value().data());
}

TEST(LeafReaderTest, IdentityIsStableAndDistinctPerReader) {
    TestLeafReader first(/*max_doc=*/1, /*num_docs=*/1, /*with_terms=*/false);
    TestLeafReader second(/*max_doc=*/1, /*num_docs=*/1, /*with_terms=*/false);

    EXPECT_TRUE(first.identity() == first.identity());
    EXPECT_FALSE(first.identity() == second.identity());

    const Identity copy = first.identity();
    EXPECT_TRUE(copy == first.identity());
    EXPECT_EQ(copy.hash(), first.identity().hash());
}

TEST(ReaderUtilTest, SubIndexFindsTheServingReader) {
    const std::vector<int32_t> doc_starts{0, 10, 20, 30};

    EXPECT_EQ(ReaderUtil::sub_index(0, doc_starts), 0);
    EXPECT_EQ(ReaderUtil::sub_index(9, doc_starts), 0);
    EXPECT_EQ(ReaderUtil::sub_index(10, doc_starts), 1);
    EXPECT_EQ(ReaderUtil::sub_index(25, doc_starts), 2);
    EXPECT_EQ(ReaderUtil::sub_index(35, doc_starts), 3);
}

TEST(ReaderUtilTest, SubIndexHandlesEmptyReaders) {
    // Two readers that serve no document each, followed by a real one: document
    // numbers map to the last reader with the same base.
    const std::vector<int32_t> doc_starts{0, 0, 5};

    EXPECT_EQ(ReaderUtil::sub_index(0, doc_starts), 1);
    EXPECT_EQ(ReaderUtil::sub_index(5, doc_starts), 2);
}

TEST(ReaderUtilTest, SubIndexWithLeavesMatchesSubIndex) {
    TestLeafReader first(/*max_doc=*/10, /*num_docs=*/10, /*with_terms=*/false);
    TestLeafReader second(/*max_doc=*/5, /*num_docs=*/5, /*with_terms=*/false);

    const Identity top{};
    const std::vector<LeafReaderContext> leaves{
            LeafReaderContext(&first, 0, 0, TopParentMeta{2, 15, top}),
            LeafReaderContext(&second, 1, 10, TopParentMeta{2, 15, top})};

    EXPECT_EQ(ReaderUtil::sub_index_with_leaves(0, leaves), 0);
    EXPECT_EQ(ReaderUtil::sub_index_with_leaves(9, leaves), 0);
    EXPECT_EQ(ReaderUtil::sub_index_with_leaves(10, leaves), 1);
    EXPECT_EQ(ReaderUtil::sub_index_with_leaves(14, leaves), 1);
}
