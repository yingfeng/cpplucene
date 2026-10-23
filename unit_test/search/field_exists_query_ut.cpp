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
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.field_exists_query;

import lucene_core;

using namespace lucene;

namespace {

/// Doc values over a sorted list of documents, so that the documents without a
/// value are simply the ones the iterator skips.
class SparseNumericDocValues final : public NumericDocValues {
public:
    SparseNumericDocValues(std::vector<std::pair<int32_t, int64_t>> values) : values_(std::move(values)) {}

    [[nodiscard]] int32_t doc_id() const override { return index_ < 0 ? -1 : doc_of(index_); }

    [[nodiscard]] Result<int32_t> next_doc() override {
        ++index_;
        return index_ >= static_cast<int32_t>(values_.size()) ? NO_MORE_DOCS : doc_of(index_);
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        while (index_ + 1 < static_cast<int32_t>(values_.size()) && doc_of(index_ + 1) < target) {
            ++index_;
        }
        auto next = next_doc();
        if (!next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
        return doc_id();
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(values_.size()); }

    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        auto advanced = advance(target);
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        return doc_id() == target;
    }

    [[nodiscard]] Result<int64_t> long_value() override {
        if (index_ < 0 || index_ >= static_cast<int32_t>(values_.size())) {
            return err(LuceneError::illegal_state("doc values are not positioned"));
        }
        return values_[static_cast<size_t>(index_)].second;
    }

private:
    [[nodiscard]] int32_t doc_of(int32_t index) const { return values_[static_cast<size_t>(index)].first; }

    std::vector<std::pair<int32_t, int64_t>> values_;
    int32_t index_{-1};
};

/// A leaf with four documents and three fields:
///   price (numeric doc values on 0 and 2), author (sorted doc values on 1 and
///   3), title (norms only), plain (nothing).
class ExistsLeafReader final : public LeafReader {
public:
    [[nodiscard]] Result<int32_t> max_doc() const override { return 4; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return 4; }

    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override { return nullptr; }
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override { return nullptr; }
    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view /*field*/) override {
        return std::unique_ptr<Terms>{};
    }

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric_doc_values(
            std::string_view field) override {
        if (field == "price") {
            return std::unique_ptr<NumericDocValues>(
                    std::make_unique<SparseNumericDocValues>(std::vector<std::pair<int32_t, int64_t>>{{0, 10},
                                                                                                      {2, 20}}));
        }
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> get_binary_doc_values(std::string_view) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted_doc_values(std::string_view field) override {
        if (field == "author") {
            return std::unique_ptr<SortedDocValues>(
                    std::make_unique<SparseSortedDocValues>(std::vector<std::pair<int32_t, int32_t>>{{1, 0}, {3, 1}},
                                                            std::vector<std::string>{"a", "b"}));
        }
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>> get_sorted_numeric_doc_values(
            std::string_view) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<SortedSetDocValues>> get_sorted_set_doc_values(std::string_view) override {
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norm_values(std::string_view field) override {
        if (field == "title") {
            return std::unique_ptr<NumericDocValues>(std::make_unique<SparseNumericDocValues>(
                    std::vector<std::pair<int32_t, int64_t>>{{0, 1}, {1, 1}, {2, 1}, {3, 1}}));
        }
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<DocValuesSkipper>> get_doc_values_skipper(std::string_view) override {
        return nullptr;
    }

    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return &field_infos_; }

    [[nodiscard]] Result<const Bits *> live_docs() const override { return static_cast<const Bits *>(nullptr); }

    /// A sorted doc values iterator over a sparse list of ordinals.
    class SparseSortedDocValues final : public SortedDocValues {
    public:
        SparseSortedDocValues(std::vector<std::pair<int32_t, int32_t>> ords, std::vector<std::string> dictionary)
                : ords_(std::move(ords)), dictionary_(std::move(dictionary)) {}

        [[nodiscard]] int32_t doc_id() const override { return index_ < 0 ? -1 : doc_of(index_); }
        [[nodiscard]] Result<int32_t> next_doc() override {
            ++index_;
            return index_ >= static_cast<int32_t>(ords_.size()) ? NO_MORE_DOCS : doc_of(index_);
        }
        [[nodiscard]] Result<int32_t> advance(int32_t target) override {
            while (index_ + 1 < static_cast<int32_t>(ords_.size()) && doc_of(index_ + 1) < target) {
                ++index_;
            }
            auto next = next_doc();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
            return doc_id();
        }
        [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(ords_.size()); }
        [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
            auto advanced = advance(target);
            if (!advanced.has_value()) {
                return std::unexpected(std::move(advanced.error()));
            }
            return doc_id() == target;
        }
        [[nodiscard]] Result<int32_t> ord_value() override {
            if (index_ < 0 || index_ >= static_cast<int32_t>(ords_.size())) {
                return -1;
            }
            return ords_[static_cast<size_t>(index_)].second;
        }
        [[nodiscard]] Result<BytesRef> lookup_ord(int32_t ord) override {
            if (ord < 0 || ord >= static_cast<int32_t>(dictionary_.size())) {
                return err(LuceneError::illegal_argument("ord out of range"));
            }
            return BytesRef::from_string(dictionary_[static_cast<size_t>(ord)]);
        }
        [[nodiscard]] Result<int32_t> get_value_count() const override {
            return static_cast<int32_t>(dictionary_.size());
        }

    private:
        [[nodiscard]] int32_t doc_of(int32_t index) const { return ords_[static_cast<size_t>(index)].first; }

        std::vector<std::pair<int32_t, int32_t>> ords_;
        std::vector<std::string> dictionary_;
        int32_t index_{-1};
    };

private:
    FieldInfos field_infos_{make_field_infos()};

    static FieldInfos make_field_infos() {
        std::vector<FieldInfo> fields(4);
        fields[0].number = 0;
        fields[0].name = "price";
        fields[0].doc_values_type = DocValuesType::Numeric;

        fields[1].number = 1;
        fields[1].name = "author";
        fields[1].doc_values_type = DocValuesType::Sorted;

        fields[2].number = 2;
        fields[2].name = "title";
        fields[2].index_options = IndexOptions::Docs;

        fields[3].number = 3;
        fields[3].name = "plain";

        auto built = FieldInfos::of(std::move(fields));
        return built.has_value() ? std::move(built.value()) : FieldInfos::none();
    }
};

IndexSearcher make_searcher() {
    std::shared_ptr<IndexReader> reader = std::make_shared<ExistsLeafReader>();
    return IndexSearcher(std::move(reader));
}

std::vector<int32_t> matching_docs(const Query &query, const IndexSearcher &searcher) {
    std::vector<int32_t> docs;
    auto weight = query.create_weight(searcher, ScoreMode::Complete, 1.0F);
    EXPECT_TRUE(weight.has_value()) << weight.error().to_string();
    if (!weight.has_value()) {
        return docs;
    }
    auto leaves = searcher.leaves();
    EXPECT_TRUE(leaves.has_value());
    if (!leaves.has_value()) {
        return docs;
    }
    for (const LeafReaderContext &context : leaves.value()) {
        auto scorer = weight.value()->scorer(context, searcher);
        EXPECT_TRUE(scorer.has_value()) << scorer.error().to_string();
        if (!scorer.has_value() || scorer.value() == nullptr) {
            continue;
        }
        auto doc = scorer.value()->next_doc();
        while (doc.has_value() && doc.value() != NO_MORE_DOCS) {
            docs.push_back(doc.value());
            doc = scorer.value()->next_doc();
        }
    }
    return docs;
}

} // namespace

TEST(FieldExistsQueryTest, MatchesTheDocumentsWithNumericDocValues) {
    const IndexSearcher searcher = make_searcher();
    FieldExistsQuery query("price");
    EXPECT_EQ(matching_docs(query, searcher), (std::vector<int32_t>{0, 2}));
}

TEST(FieldExistsQueryTest, MatchesTheDocumentsWithSortedDocValues) {
    const IndexSearcher searcher = make_searcher();
    FieldExistsQuery query("author");
    EXPECT_EQ(matching_docs(query, searcher), (std::vector<int32_t>{1, 3}));
}

TEST(FieldExistsQueryTest, MatchesTheDocumentsWithNorms) {
    const IndexSearcher searcher = make_searcher();
    FieldExistsQuery query("title");
    EXPECT_EQ(matching_docs(query, searcher), (std::vector<int32_t>{0, 1, 2, 3}));
}

TEST(FieldExistsQueryTest, AFieldThatDoesNotExistMatchesNothing) {
    const IndexSearcher searcher = make_searcher();
    FieldExistsQuery query("missing");
    EXPECT_TRUE(matching_docs(query, searcher).empty());
}

TEST(FieldExistsQueryTest, AFieldWithoutValuesOrNormsIsRejected) {
    const IndexSearcher searcher = make_searcher();
    FieldExistsQuery query("plain");
    auto weight = query.create_weight(searcher, ScoreMode::Complete, 1.0F);
    ASSERT_TRUE(weight.has_value());
    auto leaves = searcher.leaves();
    ASSERT_TRUE(leaves.has_value());
    auto supplier = weight.value()->scorer_supplier(leaves.value()[0], searcher);
    ASSERT_FALSE(supplier.has_value());
    EXPECT_EQ(supplier.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(FieldExistsQueryTest, ToStringEqualsAndHash) {
    FieldExistsQuery query("price");
    FieldExistsQuery same("price");
    FieldExistsQuery other("author");
    EXPECT_EQ(query.to_string("").value(), "FieldExistsQuery [field=price]");
    EXPECT_TRUE(query.equals(same));
    EXPECT_EQ(query.hash(), same.hash());
    EXPECT_FALSE(query.equals(other));
}

TEST(FieldExistsQueryTest, SearchAndExplain) {
    const IndexSearcher searcher = make_searcher();
    FieldExistsQuery query("price");
    auto top = searcher.search(query, 10);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    ASSERT_EQ(top.value().score_docs.size(), 2);
    EXPECT_EQ(top.value().total_hits.value, 2);
    EXPECT_FLOAT_EQ(top.value().score_docs[0].score, 1.0F);

    auto weight = query.create_weight(searcher, ScoreMode::Complete, 1.0F);
    ASSERT_TRUE(weight.has_value());
    auto leaves = searcher.leaves();
    ASSERT_TRUE(leaves.has_value());
    auto matched = weight.value()->explain(leaves.value()[0], 2, searcher);
    ASSERT_TRUE(matched.has_value());
    EXPECT_TRUE(matched.value().is_match());
    auto missed = weight.value()->explain(leaves.value()[0], 1, searcher);
    ASSERT_TRUE(missed.has_value());
    EXPECT_FALSE(missed.value().is_match());
}

// ---------------------------------------------------------------------------
// IndexOrDocValuesQuery
// ---------------------------------------------------------------------------

TEST(FieldExistsQueryTest, IndexOrDocValuesMatchesTheWrappedQuery) {
    const IndexSearcher searcher = make_searcher();
    auto query = IndexOrDocValuesQuery::create(std::make_shared<FieldExistsQuery>("price"),
                                               std::make_shared<FieldExistsQuery>("price"));
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(matching_docs(query.value(), searcher), (std::vector<int32_t>{0, 2}));
}

TEST(FieldExistsQueryTest, IndexOrDocValuesReportsBothQueries) {
    auto query = IndexOrDocValuesQuery::create(std::make_shared<FieldExistsQuery>("price"),
                                               std::make_shared<FieldExistsQuery>("author"));
    ASSERT_TRUE(query.has_value());
    EXPECT_EQ(query.value().to_string("").value(),
              "IndexOrDocValuesQuery(indexQuery=FieldExistsQuery [field=price], dvQuery=FieldExistsQuery [field=author])");
    EXPECT_TRUE(query.value().index_query()->equals(FieldExistsQuery("price")));
    EXPECT_TRUE(query.value().dv_query()->equals(FieldExistsQuery("author")));
}

TEST(FieldExistsQueryTest, IndexOrDocValuesEqualityComparesBothQueries) {
    auto left = IndexOrDocValuesQuery::create(std::make_shared<FieldExistsQuery>("price"),
                                              std::make_shared<FieldExistsQuery>("author"));
    auto same = IndexOrDocValuesQuery::create(std::make_shared<FieldExistsQuery>("price"),
                                              std::make_shared<FieldExistsQuery>("author"));
    auto other = IndexOrDocValuesQuery::create(std::make_shared<FieldExistsQuery>("price"),
                                               std::make_shared<FieldExistsQuery>("price"));
    ASSERT_TRUE(left.has_value());
    ASSERT_TRUE(same.has_value());
    ASSERT_TRUE(other.has_value());
    EXPECT_TRUE(left.value().equals(same.value()));
    EXPECT_EQ(left.value().hash(), same.value().hash());
    EXPECT_FALSE(left.value().equals(other.value()));
}

TEST(FieldExistsQueryTest, IndexOrDocValuesRejectsNullQueries) {
    auto query = IndexOrDocValuesQuery::create(nullptr, std::make_shared<FieldExistsQuery>("price"));
    ASSERT_FALSE(query.has_value());
    EXPECT_EQ(query.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(FieldExistsQueryTest, IndexOrDocValuesSearchesAndExplains) {
    const IndexSearcher searcher = make_searcher();
    auto query = IndexOrDocValuesQuery::create(std::make_shared<FieldExistsQuery>("author"),
                                               std::make_shared<FieldExistsQuery>("author"));
    ASSERT_TRUE(query.has_value());
    auto top = searcher.search(query.value(), 10);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    ASSERT_EQ(top.value().score_docs.size(), 2);
    EXPECT_EQ(top.value().score_docs[0].doc, 1);
    EXPECT_EQ(top.value().score_docs[1].doc, 3);

    auto weight = query.value().create_weight(searcher, ScoreMode::Complete, 1.0F);
    ASSERT_TRUE(weight.has_value());
    auto leaves = searcher.leaves();
    ASSERT_TRUE(leaves.has_value());
    auto explanation = weight.value()->explain(leaves.value()[0], 3, searcher);
    ASSERT_TRUE(explanation.has_value()) << explanation.error().to_string();
    EXPECT_TRUE(explanation.value().is_match());
}
