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
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.doc_values_rewrite;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kField = "tags";
constexpr int32_t kMaxDoc = 4;

/// `SortedSetDocValues` over a fixed table of ordinals; every document of the
/// table has its ords sorted, like the codec produces them.
class TableSortedSetDocValues final : public SortedSetDocValues {
public:
    TableSortedSetDocValues(std::vector<std::vector<int64_t>> ords, std::vector<std::string> dictionary)
            : ords_(std::move(ords)), dictionary_(std::move(dictionary)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        ++doc_;
        if (doc_ >= static_cast<int32_t>(ords_.size())) {
            doc_ = NO_MORE_DOCS;
        }
        // The enumeration of the ordinals restarts for every document.
        index_ = 0;
        return doc_;
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        while (doc_ != NO_MORE_DOCS && doc_ < target) {
            if (auto next = next_doc(); !next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
        }
        return doc_;
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(ords_.size()); }

    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        doc_ = target >= static_cast<int32_t>(ords_.size()) ? NO_MORE_DOCS : target;
        index_ = 0;
        if (doc_ == NO_MORE_DOCS) {
            return false;
        }
        return !ords_[static_cast<size_t>(doc_)].empty();
    }

    [[nodiscard]] Result<int64_t> next_ord() override {
        const auto &ords = ords_[static_cast<size_t>(doc_)];
        if (index_ >= static_cast<int32_t>(ords.size())) {
            return NO_MORE_ORDS;
        }
        return ords[static_cast<size_t>(index_++)];
    }

    [[nodiscard]] Result<int32_t> doc_value_count() override {
        return static_cast<int32_t>(ords_[static_cast<size_t>(doc_)].size());
    }

    [[nodiscard]] Result<BytesRef> lookup_ord(int64_t ord) override {
        if (ord < 0 || static_cast<size_t>(ord) >= dictionary_.size()) {
            return err(LuceneError::illegal_argument("ord out of range"));
        }
        return BytesRef::from_string(dictionary_[static_cast<size_t>(ord)]);
    }

    [[nodiscard]] Result<int64_t> get_value_count() const override {
        return static_cast<int64_t>(dictionary_.size());
    }

private:
    std::vector<std::vector<int64_t>> ords_;
    std::vector<std::string> dictionary_;
    int32_t doc_{-1};
    int32_t index_{0};
};

/// A leaf with doc values but no postings: exactly what the doc values rewrite
/// exists for.
class DocValuesLeafReader final : public LeafReader {
public:
    [[nodiscard]] Result<int32_t> max_doc() const override { return kMaxDoc; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return kMaxDoc; }

    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override { return nullptr; }
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override { return nullptr; }

    /// No postings at all.
    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view /*field*/) override {
        return std::unique_ptr<Terms>{};
    }

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric_doc_values(
            std::string_view /*field*/) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> get_binary_doc_values(
            std::string_view /*field*/) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted_doc_values(
            std::string_view /*field*/) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>> get_sorted_numeric_doc_values(
            std::string_view /*field*/) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<SortedSetDocValues>> get_sorted_set_doc_values(
            std::string_view field) override {
        if (field != kField) {
            return nullptr;
        }
        // alpha | (alpha, gamma) | (none) | (alpha, beta)
        return std::unique_ptr<SortedSetDocValues>(new TableSortedSetDocValues(
                std::vector<std::vector<int64_t>>{{0}, {0, 2}, {}, {0, 1}},
                std::vector<std::string>{"alpha", "beta", "gamma"}));
    }
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norm_values(std::string_view /*field*/) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<DocValuesSkipper>> get_doc_values_skipper(
            std::string_view /*field*/) override {
        return nullptr;
    }

    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return &field_infos_; }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return static_cast<const Bits *>(nullptr); }

private:
    FieldInfos field_infos_{FieldInfos::none()};
};

/// An [`IndexReader`] with the doc values leaf as its only leaf.
class DocValuesIndexReader final : public IndexReader {
public:
    DocValuesIndexReader() : leaf_(new DocValuesLeafReader()) {
        leaves_.emplace_back(leaf_.get(), 0, 0, TopParentMeta{1, kMaxDoc, identity_});
    }

    [[nodiscard]] Result<int32_t> max_doc() const override { return kMaxDoc; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return kMaxDoc; }
    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override { return nullptr; }
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override { return nullptr; }
    [[nodiscard]] Result<int32_t> doc_freq(const Term & /*term*/) override { return 0; }
    [[nodiscard]] Result<int64_t> total_term_freq(const Term & /*term*/) override { return 0; }
    [[nodiscard]] Result<int64_t> sum_doc_freq(std::string_view /*field*/) override { return 0; }
    [[nodiscard]] Result<int32_t> doc_count(std::string_view /*field*/) override { return 0; }
    [[nodiscard]] Result<int64_t> sum_total_term_freq(std::string_view /*field*/) override { return 0; }
    [[nodiscard]] Result<std::span<const LeafReaderContext>> leaves() const override {
        return std::span<const LeafReaderContext>(leaves_);
    }
    [[nodiscard]] const Identity &identity() const override { return identity_; }

private:
    Identity identity_{};
    std::unique_ptr<DocValuesLeafReader> leaf_;
    std::vector<LeafReaderContext> leaves_{};
};

/// Drains the scorer of `query` over the only leaf.
std::vector<int32_t> collect(IndexSearcher &searcher, const Query &query) {
    std::vector<int32_t> docs;
    auto weight = query.create_weight(searcher, ScoreMode::TopScores, 1.0F);
    EXPECT_TRUE(weight.has_value()) << (weight.has_value() ? "" : weight.error().to_string());
    if (!weight.has_value()) {
        return docs;
    }
    auto leaves = searcher.leaves();
    EXPECT_TRUE(leaves.has_value());
    if (!leaves.has_value()) {
        return docs;
    }
    auto scorer = weight.value()->scorer(leaves.value().front(), searcher);
    EXPECT_TRUE(scorer.has_value()) << (scorer.has_value() ? "" : scorer.error().to_string());
    if (!scorer.has_value() || scorer.value() == nullptr) {
        return docs;
    }
    while (true) {
        auto doc = scorer.value()->next_doc();
        EXPECT_TRUE(doc.has_value()) << (doc.has_value() ? "" : doc.error().to_string());
        if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
            break;
        }
        docs.push_back(doc.value());
    }
    return docs;
}

std::vector<int32_t> collect_with_doc_values_rewrite(IndexSearcher &searcher, const Query &query) {
    auto rewritten = searcher.rewrite(query);
    EXPECT_TRUE(rewritten.has_value()) << (rewritten.has_value() ? "" : rewritten.error().to_string());
    if (!rewritten.has_value() || rewritten.value() == nullptr) {
        return {};
    }
    return collect(searcher, *rewritten.value());
}

} // namespace

TEST(DocValuesRewriteTest, PrefixQueryMatchesThroughDocValues) {
    IndexSearcher searcher(std::make_shared<DocValuesIndexReader>());
    auto query = std::make_shared<PrefixQuery>(std::string(kField), "al", MultiTermRewriteMethod::kDocValues);
    // "alpha" is the only term with that prefix, and the documents 0, 1 and 3
    // carry it.
    EXPECT_EQ(collect_with_doc_values_rewrite(searcher, *query), (std::vector<int32_t>{0, 1, 3}));
}

TEST(DocValuesRewriteTest, WildcardQueryMatchesThroughDocValues) {
    IndexSearcher searcher(std::make_shared<DocValuesIndexReader>());
    auto built = WildcardQuery::with_rewrite(Term::from_text(std::string(kField), "*a"), 10000,
                                             MultiTermRewriteMethod::kDocValues);
    ASSERT_TRUE(built.has_value()) << built.error().to_string();
    // The doc values wrapper keeps the query alive, so it must be owned by a
    // shared pointer.
    auto query = std::make_shared<WildcardQuery>(std::move(built.value()));
    // "alpha" and "beta" end in a; "gamma" does not.
    EXPECT_EQ(collect_with_doc_values_rewrite(searcher, *query), (std::vector<int32_t>{0, 1, 3}));
}

TEST(DocValuesRewriteTest, TermRangeQueryMatchesThroughDocValues) {
    IndexSearcher searcher(std::make_shared<DocValuesIndexReader>());
    auto built = TermRangeQuery::with_string_range_rewrite(std::string(kField), std::string("beta"),
                                                          std::string("gamma"), true, true,
                                                          MultiTermRewriteMethod::kDocValues);
    ASSERT_TRUE(built.has_value()) << built.error().to_string();
    auto query = std::make_shared<TermRangeQuery>(std::move(built.value()));
    EXPECT_EQ(collect_with_doc_values_rewrite(searcher, *query), (std::vector<int32_t>{1, 3}));
}

TEST(DocValuesRewriteTest, EmptyResultWhenNoTermMatches) {
    IndexSearcher searcher(std::make_shared<DocValuesIndexReader>());
    auto query = std::make_shared<PrefixQuery>(std::string(kField), "zz", MultiTermRewriteMethod::kDocValues);
    auto rewritten = searcher.rewrite(*query);
    ASSERT_TRUE(rewritten.has_value());
    EXPECT_TRUE(collect(searcher, *rewritten.value()).empty());
}

TEST(DocValuesRewriteTest, UnknownFieldMatchesNothing) {
    IndexSearcher searcher(std::make_shared<DocValuesIndexReader>());
    auto query = std::make_shared<PrefixQuery>("other", "al", MultiTermRewriteMethod::kDocValues);
    auto rewritten = searcher.rewrite(*query);
    ASSERT_TRUE(rewritten.has_value());
    EXPECT_TRUE(collect(searcher, *rewritten.value()).empty());
}

TEST(DocValuesRewriteTest, RewritesIntoAConstantScoreQueryOverTheWrapper) {
    IndexSearcher searcher(std::make_shared<DocValuesIndexReader>());
    auto query = std::make_shared<PrefixQuery>(std::string(kField), "al", MultiTermRewriteMethod::kDocValues);
    auto rewritten = query->rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value()) << rewritten.error().to_string();
    const auto *constant_score = dynamic_cast<const ConstantScoreQuery *>(rewritten.value().get());
    ASSERT_NE(constant_score, nullptr);
    const auto *wrapper = dynamic_cast<const MultiTermQueryDocValuesWrapper *>(constant_score->query().get());
    ASSERT_NE(wrapper, nullptr);
    EXPECT_EQ(wrapper->field(), kField);
    EXPECT_EQ(wrapper->to_string("").value(), query->to_string("").value());
}

TEST(DocValuesRewriteTest, StackAllocatedQueryCannotUseTheDocValuesRewrite) {
    // The wrapper keeps the query alive, so it needs shared ownership.
    const PrefixQuery query(std::string(kField), "al", MultiTermRewriteMethod::kDocValues);
    IndexSearcher searcher(std::make_shared<DocValuesIndexReader>());
    auto rewritten = query.rewrite(searcher);
    EXPECT_FALSE(rewritten.has_value());
}

TEST(DocValuesRewriteTest, SortedSetDocValuesTermsEnumWalksTheDictionary) {
    TableSortedSetDocValues values({{0}, {0, 2}, {}, {0, 1}},
                                  std::vector<std::string>{"alpha", "beta", "gamma"});
    auto shared = std::make_shared<TableSortedSetDocValues>(std::move(values));
    SortedSetDocValuesTermsEnum terms_enum(shared);

    std::vector<std::string> terms;
    std::vector<int64_t> ords;
    while (true) {
        auto next = terms_enum.next();
        ASSERT_TRUE(next.has_value());
        if (next.value() == nullptr) {
            break;
        }
        terms.push_back(next.value()->utf8_to_string().value());
        ords.push_back(terms_enum.ord().value());
    }
    EXPECT_EQ(terms, (std::vector<std::string>{"alpha", "beta", "gamma"}));
    EXPECT_EQ(ords, (std::vector<int64_t>{0, 1, 2}));

    EXPECT_TRUE(terms_enum.seek_exact(BytesRef::from_string("beta").as_slice()).value());
    EXPECT_EQ(terms_enum.ord().value(), 1);
    EXPECT_FALSE(terms_enum.seek_exact(BytesRef::from_string("delta").as_slice()).value());
    EXPECT_EQ(terms_enum.seek_ceil(BytesRef::from_string("bb").as_slice()).value(), SeekStatus::NotFound);
    EXPECT_EQ(terms_enum.ord().value(), 1);
    EXPECT_EQ(terms_enum.seek_ceil(BytesRef::from_string("zz").as_slice()).value(), SeekStatus::End);

    ASSERT_TRUE(terms_enum.seek_exact_with_ord(2).has_value());
    EXPECT_EQ(terms_enum.term().value()->utf8_to_string().value(), "gamma");
    EXPECT_FALSE(terms_enum.seek_exact_with_ord(3).has_value());
}
