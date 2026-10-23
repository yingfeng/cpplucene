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
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.sorted_numeric_doc_values_set_query;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kField = "num";

/// Doc values with one value list per document.
class FakeSortedNumeric final : public SortedNumericDocValues {
public:
    FakeSortedNumeric(std::vector<std::vector<int64_t>> values, bool single_valued)
            : values_(std::move(values)), single_valued_(single_valued) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override { return advance(doc_ + 1); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        doc_ = static_cast<size_t>(target) < values_.size() ? target : NO_MORE_DOCS;
        index_ = 0;
        return doc_;
    }
    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(values_.size()); }

    [[nodiscard]] Result<int64_t> next_value() override {
        const auto &values = values_[static_cast<size_t>(doc_)];
        if (index_ >= values.size()) {
            return err(LuceneError::illegal_state("no more values for this document"));
        }
        return values[index_++];
    }

    [[nodiscard]] Result<int32_t> doc_value_count() override {
        return static_cast<int32_t>(values_[static_cast<size_t>(doc_)].size());
    }

    [[nodiscard]] bool is_single_valued() const override { return single_valued_; }

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric_doc_values() override {
        return err(LuceneError::unsupported_operation("the fake has no numeric view"));
    }

private:
    std::vector<std::vector<int64_t>> values_;
    bool single_valued_;
    int32_t doc_{-1};
    size_t index_{0};
};

/// Numeric doc values with one value per document, used for the single-valued
/// path.
class FakeNumeric final : public NumericDocValues {
public:
    explicit FakeNumeric(std::vector<int64_t> values) : values_(std::move(values)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override { return advance(doc_ + 1); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        doc_ = static_cast<size_t>(target) < values_.size() ? target : NO_MORE_DOCS;
        return doc_;
    }
    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(values_.size()); }
    [[nodiscard]] Result<int64_t> long_value() override {
        if (doc_ < 0 || static_cast<size_t>(doc_) >= values_.size()) {
            return err(LuceneError::illegal_state("the doc values are not positioned"));
        }
        return values_[static_cast<size_t>(doc_)];
    }

private:
    std::vector<int64_t> values_;
    int32_t doc_{-1};
};

/// A leaf reader with one doc values field.
class SetQueryLeafReader final : public LeafReader {
public:
    SetQueryLeafReader(int32_t max_doc, std::vector<std::vector<int64_t>> sorted_numeric, bool single_valued)
            : max_doc_(max_doc), sorted_numeric_(std::move(sorted_numeric)), single_valued_(single_valued) {
        values_.reserve(sorted_numeric_.size());
        for (const auto &doc_values : sorted_numeric_) {
            EXPECT_FALSE(doc_values.empty());
            values_.push_back(doc_values.front());
        }
        FieldInfo info;
        info.number = 0;
        info.name = std::string(kField);
        info.doc_values_type = DocValuesType::SortedNumeric;
        auto built = FieldInfos::of({info});
        EXPECT_TRUE(built.has_value());
        field_infos_ = std::move(built.value());
    }

    [[nodiscard]] Result<int32_t> max_doc() const override { return max_doc_; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return max_doc_; }

    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override {
        return std::unique_ptr<StoredFields>{};
    }
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override {
        return std::unique_ptr<TermVectors>{};
    }
    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view /*field*/) override {
        return std::unique_ptr<Terms>{};
    }

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric_doc_values(
            std::string_view field) override {
        if (field != kField) {
            return std::unique_ptr<NumericDocValues>{};
        }
        return std::unique_ptr<NumericDocValues>(std::make_unique<FakeNumeric>(values_));
    }
    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> get_binary_doc_values(std::string_view /*field*/) override {
        return std::unique_ptr<BinaryDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted_doc_values(std::string_view /*field*/) override {
        return std::unique_ptr<SortedDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>>
    get_sorted_numeric_doc_values(std::string_view field) override {
        if (field != kField) {
            return std::unique_ptr<SortedNumericDocValues>{};
        }
        return std::unique_ptr<SortedNumericDocValues>(
                std::make_unique<FakeSortedNumeric>(sorted_numeric_, single_valued_));
    }
    [[nodiscard]] Result<std::unique_ptr<SortedSetDocValues>>
    get_sorted_set_doc_values(std::string_view /*field*/) override {
        return std::unique_ptr<SortedSetDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norm_values(std::string_view /*field*/) override {
        return std::unique_ptr<NumericDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<DocValuesSkipper>> get_doc_values_skipper(
            std::string_view /*field*/) override {
        return std::unique_ptr<DocValuesSkipper>{};
    }

    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return &field_infos_; }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return static_cast<const Bits *>(nullptr); }

private:
    int32_t max_doc_;
    std::vector<std::vector<int64_t>> sorted_numeric_;
    bool single_valued_;
    std::vector<int64_t> values_;
    FieldInfos field_infos_{FieldInfos::none()};
};

/// The documents the query matches in `reader`.
std::vector<int32_t> matching_docs(const SortedNumericDocValuesSetQuery &query, IndexSearcher &searcher) {
    std::vector<int32_t> docs;
    auto weight = query.create_weight(searcher, ScoreMode::CompleteNoScores, 1.0F);
    EXPECT_TRUE(weight.has_value());
    auto leaves = searcher.leaves();
    EXPECT_TRUE(leaves.has_value());
    auto supplier = weight.value()->scorer_supplier(leaves.value()[0], searcher);
    if (!supplier.has_value() || supplier.value() == nullptr) {
        return docs;
    }
    auto scorer = supplier.value()->get(1, leaves.value()[0], searcher);
    if (!scorer.has_value() || scorer.value() == nullptr) {
        return docs;
    }
    while (true) {
        auto doc = scorer.value()->next_doc();
        if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
            break;
        }
        if (scorer.value()->has_two_phase_iterator() == TwoPhaseState::Yes) {
            auto matches = scorer.value()->matches();
            if (!matches.value()) {
                continue;
            }
        }
        docs.push_back(doc.value());
    }
    return docs;
}

} // namespace

TEST(SortedNumericDocValuesSetQueryTest, MatchesMultiValuedDocuments) {
    auto reader = std::make_shared<SetQueryLeafReader>(
            3, std::vector<std::vector<int64_t>>{{1, 2}, {3}, {4}}, false);
    IndexSearcher searcher(reader);

    auto query = SortedNumericDocValuesSetQuery::create(std::string(kField), std::vector<int64_t>{3, 2});
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(matching_docs(query.value(), searcher), (std::vector<int32_t>{0, 1}));
}

TEST(SortedNumericDocValuesSetQueryTest, MatchesSingleValuedDocuments) {
    auto reader = std::make_shared<SetQueryLeafReader>(
            3, std::vector<std::vector<int64_t>>{{7}, {8}, {9}}, true);
    IndexSearcher searcher(reader);

    auto query = SortedNumericDocValuesSetQuery::create(std::string(kField), std::vector<int64_t>{9, 7});
    ASSERT_TRUE(query.has_value());
    EXPECT_EQ(matching_docs(query.value(), searcher), (std::vector<int32_t>{0, 2}));
}

TEST(SortedNumericDocValuesSetQueryTest, RewritesAnEmptySetToMatchNoDocs) {
    auto query = SortedNumericDocValuesSetQuery::create(std::string(kField), {});
    ASSERT_TRUE(query.has_value());

    IndexSearcher searcher(std::make_shared<SetQueryLeafReader>(1, std::vector<std::vector<int64_t>>{{1}}, false));
    auto rewritten = query.value().rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value()) << rewritten.error().to_string();
    ASSERT_TRUE(rewritten.value() != nullptr);
    EXPECT_TRUE(dynamic_cast<const MatchNoDocsQuery *>(rewritten.value().get()) != nullptr);
}

TEST(SortedNumericDocValuesSetQueryTest, MissingFieldMatchesNothing) {
    auto reader = std::make_shared<SetQueryLeafReader>(1, std::vector<std::vector<int64_t>>{{1}}, false);
    IndexSearcher searcher(reader);

    auto query = SortedNumericDocValuesSetQuery::create("other", std::vector<int64_t>{1});
    ASSERT_TRUE(query.has_value());
    EXPECT_TRUE(matching_docs(query.value(), searcher).empty());
}

TEST(SortedNumericDocValuesSetQueryTest, DescribesAndComparesItself) {
    auto first = SortedNumericDocValuesSetQuery::create(std::string(kField), std::vector<int64_t>{2, 1});
    ASSERT_TRUE(first.has_value());
    auto second = SortedNumericDocValuesSetQuery::create(std::string(kField), std::vector<int64_t>{1, 2});
    ASSERT_TRUE(second.has_value());
    auto other = SortedNumericDocValuesSetQuery::create(std::string(kField), std::vector<int64_t>{3});
    ASSERT_TRUE(other.has_value());

    EXPECT_TRUE(first.value().equals(second.value()));
    EXPECT_FALSE(first.value().equals(other.value()));
    EXPECT_EQ(first.value().hash(), second.value().hash());
    EXPECT_EQ(first.value().numbers().size(), 2U);

    auto rendered = first.value().to_string("");
    ASSERT_TRUE(rendered.has_value());
    EXPECT_EQ(rendered.value().substr(0, std::string(kField).size()), std::string(kField));
}
