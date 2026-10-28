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

module lucene_core:ut.sort;

import lucene_core;

using namespace lucene;

namespace {

// ---------------------------------------------------------------------------
// doc values over arrays
// ---------------------------------------------------------------------------

class ArrayNumericDocValues final : public NumericDocValues {
public:
    explicit ArrayNumericDocValues(std::vector<int64_t> values) : values_(std::move(values)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        ++doc_;
        return doc_ < static_cast<int32_t>(values_.size()) ? doc_ : NO_MORE_DOCS;
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        while (doc_ < target) {
            auto next = next_doc();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
        }
        return doc_;
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(values_.size()); }

    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        auto advanced = advance(target);
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        return doc_ == target && doc_ < static_cast<int32_t>(values_.size());
    }

    [[nodiscard]] Result<int64_t> long_value() override {
        if (doc_ < 0 || doc_ >= static_cast<int32_t>(values_.size())) {
            return err(LuceneError::illegal_state("doc values are not positioned"));
        }
        return values_[static_cast<size_t>(doc_)];
    }

private:
    std::vector<int64_t> values_;
    int32_t doc_{-1};
};

class SortedArrayDocValues final : public SortedDocValues {
public:
    /// `values[d]` is the ordinal of document `d`; the dictionary holds the
    /// sorted distinct ordinals as strings.
    SortedArrayDocValues(std::vector<int32_t> ords, std::vector<std::string> dictionary)
            : ords_(std::move(ords)), dictionary_(std::move(dictionary)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        ++doc_;
        return doc_ < static_cast<int32_t>(ords_.size()) ? doc_ : NO_MORE_DOCS;
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        while (doc_ < target) {
            auto next = next_doc();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
        }
        return doc_;
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(ords_.size()); }

    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        auto advanced = advance(target);
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        return doc_ == target && doc_ < static_cast<int32_t>(ords_.size());
    }

    [[nodiscard]] Result<int32_t> ord_value() override {
        if (doc_ < 0 || doc_ >= static_cast<int32_t>(ords_.size())) {
            return -1;
        }
        return ords_[static_cast<size_t>(doc_)];
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
    std::vector<int32_t> ords_;
    std::vector<std::string> dictionary_;
    int32_t doc_{-1};
};

/// Multi-valued long doc values over a fixed table.
class ArraySortedNumericDocValues final : public SortedNumericDocValues {
public:
    explicit ArraySortedNumericDocValues(std::vector<std::vector<int64_t>> values)
            : values_(std::move(values)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        ++doc_;
        if (doc_ >= static_cast<int32_t>(values_.size())) {
            doc_ = NO_MORE_DOCS;
        }
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

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(values_.size()); }

    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        doc_ = target >= static_cast<int32_t>(values_.size()) ? NO_MORE_DOCS : target;
        return advance_exact_value();
    }

    [[nodiscard]] Result<int64_t> next_value() override {
        auto &values = values_[static_cast<size_t>(doc_)];
        if (index_ >= static_cast<int32_t>(values.size())) {
            return err(LuceneError::illegal_state("no more values in this document"));
        }
        return values[static_cast<size_t>(index_++)];
    }

    [[nodiscard]] Result<int32_t> doc_value_count() override {
        return static_cast<int32_t>(values_[static_cast<size_t>(doc_)].size());
    }

private:
    [[nodiscard]] Result<bool> advance_exact_value() {
        index_ = 0;
        if (doc_ == NO_MORE_DOCS) {
            return false;
        }
        return !values_[static_cast<size_t>(doc_)].empty();
    }

    std::vector<std::vector<int64_t>> values_;
    int32_t doc_{-1};
    int32_t index_{0};
};

/// Multi-valued term doc values over a fixed table of ordinals.
class ArraySortedSetDocValues final : public SortedSetDocValues {
public:
    ArraySortedSetDocValues(std::vector<std::vector<int64_t>> ords, std::vector<std::string> dictionary)
            : ords_(std::move(ords)), dictionary_(std::move(dictionary)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        ++doc_;
        if (doc_ >= static_cast<int32_t>(ords_.size())) {
            doc_ = NO_MORE_DOCS;
        }
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
        auto &ords = ords_[static_cast<size_t>(doc_)];
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

/// A leaf with four documents and two doc values fields:
///   price  = 30, 10, 20, 10
///   author = b,  a,  c,  a
class SortLeafReader final : public LeafReader {
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
                    std::make_unique<ArrayNumericDocValues>(std::vector<int64_t>{30, 10, 20, 10}));
        }
        return nullptr;
    }

    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> get_binary_doc_values(
            std::string_view /*field*/) override {
        return nullptr;
    }

    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted_doc_values(
            std::string_view field) override {
        if (field == "author") {
            return std::unique_ptr<SortedDocValues>(std::make_unique<SortedArrayDocValues>(
                    std::vector<int32_t>{1, 0, 2, 0}, std::vector<std::string>{"a", "b", "c"}));
        }
        return nullptr;
    }

    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>> get_sorted_numeric_doc_values(
            std::string_view field) override {
        if (field == "sizes") {
            return std::unique_ptr<SortedNumericDocValues>(std::make_unique<ArraySortedNumericDocValues>(
                    std::vector<std::vector<int64_t>>{{10, 40}, {5}, {}, {30, 20}}));
        }
        if (field == "ratio") {
            const auto bits = [](double value) { return std::bit_cast<int64_t>(value); };
            return std::unique_ptr<SortedNumericDocValues>(std::make_unique<ArraySortedNumericDocValues>(
                    std::vector<std::vector<int64_t>>{{bits(2.5), bits(0.5)}, {bits(9.0)}, {},
                                                      {bits(1.0), bits(3.0)}}));
        }
        return nullptr;
    }
    [[nodiscard]] Result<std::unique_ptr<SortedSetDocValues>> get_sorted_set_doc_values(
            std::string_view field) override {
        if (field == "tags") {
            return std::unique_ptr<SortedSetDocValues>(std::make_unique<ArraySortedSetDocValues>(
                    std::vector<std::vector<int64_t>>{{0, 2}, {1}, {}, {0, 1}},
                    std::vector<std::string>{"alpha", "beta", "gamma"}));
        }
        return nullptr;
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

// ---------------------------------------------------------------------------
// a query that matches every document, scored by doc number
// ---------------------------------------------------------------------------

class AllDocsScorer final : public Scorer {
public:
    explicit AllDocsScorer(int32_t max_doc) : max_doc_(max_doc) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override {
        ++doc_;
        return doc_ < max_doc_ ? doc_ : NO_MORE_DOCS;
    }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        while (doc_ < target) {
            auto next = next_doc();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
        }
        return doc_;
    }
    [[nodiscard]] Result<int64_t> cost() const override { return max_doc_; }
    [[nodiscard]] Result<int32_t> scoring_doc_id() override { return doc_; }
    [[nodiscard]] Result<float> get_max_score(int32_t /*upto*/) override {
        return std::numeric_limits<float>::infinity();
    }
    [[nodiscard]] TwoPhaseState has_two_phase_iterator() const override { return TwoPhaseState::No; }
    [[nodiscard]] Result<float> score() override { return static_cast<float>((doc_ + 1) * 10); }

private:
    int32_t max_doc_{0};
    int32_t doc_{-1};
};

class AllDocsScorerSupplier final : public ScorerSupplier {
public:
    explicit AllDocsScorerSupplier(int32_t max_doc) : max_doc_(max_doc) {}

    [[nodiscard]] Result<std::unique_ptr<Scorer>> get(int64_t /*lead_cost*/, const LeafReaderContext & /*context*/,
                                                      const IndexSearcher & /*searcher*/) override {
        return std::unique_ptr<Scorer>(new AllDocsScorer(max_doc_));
    }
    [[nodiscard]] Result<int64_t> cost(const LeafReaderContext & /*context*/,
                                       const IndexSearcher & /*searcher*/) override {
        return max_doc_;
    }

private:
    int32_t max_doc_{0};
};

class AllDocsQuery;

class AllDocsWeight final : public Weight {
public:
    explicit AllDocsWeight(const AllDocsQuery &query) : query_(query) {}

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext & /*context*/, int32_t /*doc*/,
                                              const IndexSearcher & /*searcher*/) const override {
        return Explanation::no_match_no_details("AllDocsWeight does not explain");
    }
    [[nodiscard]] const Query &get_query() const override;
    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext &context,
                                                                         const IndexSearcher & /*searcher*/) override {
        return std::unique_ptr<ScorerSupplier>(
                std::make_unique<AllDocsScorerSupplier>(context.reader()->max_doc().value()));
    }
    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext & /*context*/) const override { return true; }

private:
    const AllDocsQuery &query_;
};

class AllDocsQuery final : public Query {
public:
    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher & /*searcher*/,
                                                               ScoreMode /*score_mode*/,
                                                               float /*boost*/) const override {
        return std::unique_ptr<Weight>(new AllDocsWeight(*this));
    }
    [[nodiscard]] VoidResult visit(QueryVisitor & /*visitor*/) const override { return {}; }
    [[nodiscard]] Result<std::string> to_string(std::string_view /*field*/) const override {
        return std::string("AllDocs");
    }
    [[nodiscard]] bool equals(const Query &other) const override {
        return dynamic_cast<const AllDocsQuery *>(&other) != nullptr;
    }
    [[nodiscard]] size_t hash() const override { return 42; }
};

const Query &AllDocsWeight::get_query() const { return query_; }

IndexSearcher make_searcher() {
    std::shared_ptr<IndexReader> reader = std::make_shared<SortLeafReader>();
    return IndexSearcher(std::move(reader));
}

std::vector<int32_t> hit_docs(const TopFieldDocs &top) {
    std::vector<int32_t> docs;
    for (const FieldDoc &hit : top.score_docs) {
        docs.push_back(hit.doc);
    }
    return docs;
}

// ---------------------------------------------------------------------------
// sort fields
// ---------------------------------------------------------------------------

TEST(SortTest, SortFieldToString) {
    auto score = SortField::score();
    ASSERT_TRUE(score.has_value());
    EXPECT_EQ(score.value().to_string().value(), "<score>");

    auto doc = SortField::doc();
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc.value().to_string().value(), "<doc>");

    auto reversed = SortField::with_reverse("price", SortFieldType::kInt, true);
    ASSERT_TRUE(reversed.has_value());
    EXPECT_EQ(reversed.value().to_string().value(), "<int: \"price\">!");

    auto text = SortField::with_reverse("author", SortFieldType::kString, false);
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text.value().to_string().value(), "<string: \"author\">");
}

TEST(SortTest, SortFieldNeedsAFieldUnlessItSortsOnScoreOrDoc) {
    EXPECT_TRUE(SortField::create(std::nullopt, SortFieldType::kScore).has_value());
    EXPECT_TRUE(SortField::create(std::nullopt, SortFieldType::kDoc).has_value());

    auto string_without_field = SortField::create(std::nullopt, SortFieldType::kString);
    ASSERT_FALSE(string_without_field.has_value());
    EXPECT_EQ(string_without_field.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(SortTest, SortFieldNeedsScores) {
    EXPECT_TRUE(SortField::score().value().needs_scores());
    EXPECT_FALSE(SortField::doc().value().needs_scores());
    EXPECT_FALSE(SortField::create("price", SortFieldType::kInt).value().needs_scores());
}

TEST(SortTest, SortNeedsAtLeastOneField) {
    auto empty = Sort::with_fields({});
    ASSERT_FALSE(empty.has_value());
    EXPECT_EQ(empty.error().kind(), ErrorKind::kIllegalArgument);

    EXPECT_TRUE(Sort::relevance().has_value());
    EXPECT_TRUE(Sort::index_order().has_value());
}

TEST(SortTest, SortToStringJoinsTheFields) {
    std::vector<SortField> fields{SortField::with_reverse("author", SortFieldType::kString, false).value(),
                                  SortField::with_reverse("price", SortFieldType::kInt, true).value()};
    auto sort = Sort::with_fields(std::move(fields));
    ASSERT_TRUE(sort.has_value());
    EXPECT_EQ(sort.value().to_string(), "<string: \"author\">,<int: \"price\">!");
    EXPECT_TRUE(sort.value().needs_scores() == false);
    EXPECT_TRUE(Sort::relevance().value().needs_scores());
}

TEST(SortTest, SortEqualsAndHash) {
    auto left = Sort::with_fields({SortField::create("price", SortFieldType::kInt).value()});
    auto same = Sort::with_fields({SortField::create("price", SortFieldType::kInt).value()});
    auto other = Sort::with_fields({SortField::create("author", SortFieldType::kString).value()});
    ASSERT_TRUE(left.has_value());
    ASSERT_TRUE(same.has_value());
    ASSERT_TRUE(other.has_value());
    EXPECT_TRUE(left.value().equals(same.value()));
    EXPECT_EQ(left.value().hash(), same.value().hash());
    EXPECT_FALSE(left.value().equals(other.value()));
}

// ---------------------------------------------------------------------------
// sorted searches
// ---------------------------------------------------------------------------

TEST(SortTest, SortsByNumericFieldAscending) {
    const IndexSearcher searcher = make_searcher();
    auto sort = Sort::with_fields({SortField::create("price", SortFieldType::kInt).value()});
    ASSERT_TRUE(sort.has_value());
    auto top = searcher.search(AllDocsQuery(), sort.value(), 10);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    // 30, 10, 20, 10 -> the two 10s are ordered by document number.
    EXPECT_EQ(hit_docs(top.value()), (std::vector<int32_t>{1, 3, 2, 0}));
    EXPECT_EQ(top.value().total_hits.value, 4);
    EXPECT_EQ(top.value().total_hits.relation, TotalHits::Relation::EqualTo);
}

TEST(SortTest, SortsByNumericFieldDescending) {
    const IndexSearcher searcher = make_searcher();
    auto sort = Sort::with_fields({SortField::with_reverse("price", SortFieldType::kInt, true).value()});
    ASSERT_TRUE(sort.has_value());
    auto top = searcher.search(AllDocsQuery(), sort.value(), 10);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    EXPECT_EQ(hit_docs(top.value()), (std::vector<int32_t>{0, 2, 1, 3}));
}

TEST(SortTest, SortsByTermField) {
    const IndexSearcher searcher = make_searcher();
    auto sort = Sort::with_fields({SortField::create("author", SortFieldType::kString).value()});
    ASSERT_TRUE(sort.has_value());
    auto top = searcher.search(AllDocsQuery(), sort.value(), 10);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    // a, a, b, c
    EXPECT_EQ(hit_docs(top.value()), (std::vector<int32_t>{1, 3, 0, 2}));
    EXPECT_EQ(std::get<std::string>(top.value().score_docs[0].fields[0]), "a");
}

TEST(SortTest, SortsByTermFieldDescending) {
    const IndexSearcher searcher = make_searcher();
    auto sort = Sort::with_fields({SortField::with_reverse("author", SortFieldType::kString, true).value()});
    ASSERT_TRUE(sort.has_value());
    auto top = searcher.search(AllDocsQuery(), sort.value(), 10);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    EXPECT_EQ(hit_docs(top.value()), (std::vector<int32_t>{2, 0, 1, 3}));
}

TEST(SortTest, RelevanceSortPutsTheHighestScoreFirst) {
    const IndexSearcher searcher = make_searcher();
    auto top = searcher.search(AllDocsQuery(), Sort::relevance().value(), 10);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    EXPECT_EQ(hit_docs(top.value()), (std::vector<int32_t>{3, 2, 1, 0}));
    // Four hits stay below the total-hits threshold, so the count is exact.
    EXPECT_EQ(top.value().total_hits.relation, TotalHits::Relation::EqualTo);
    EXPECT_EQ(top.value().total_hits.value, 4);
    EXPECT_FLOAT_EQ(top.value().score_docs.front().score, 40.0F);
}

TEST(SortTest, IndexOrderSortPutsTheLowestDocumentFirst) {
    const IndexSearcher searcher = make_searcher();
    auto top = searcher.search(AllDocsQuery(), Sort::index_order().value(), 10);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    EXPECT_EQ(hit_docs(top.value()), (std::vector<int32_t>{0, 1, 2, 3}));
}

TEST(SortTest, MissingValuesSortAsZeroAndEmptyString) {
    const IndexSearcher searcher = make_searcher();
    // The field is missing on the leaf: every value is zero, so the order is the
    // document order.
    auto sort = Sort::with_fields({SortField::create("absent", SortFieldType::kLong).value()});
    ASSERT_TRUE(sort.has_value());
    auto top = searcher.search(AllDocsQuery(), sort.value(), 10);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    EXPECT_EQ(hit_docs(top.value()), (std::vector<int32_t>{0, 1, 2, 3}));
}

TEST(SortTest, KeepsOnlyTheRequestedNumberOfHits) {
    const IndexSearcher searcher = make_searcher();
    auto sort = Sort::with_fields({SortField::create("price", SortFieldType::kInt).value()});
    auto top = searcher.search(AllDocsQuery(), sort.value(), 2);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    EXPECT_EQ(hit_docs(top.value()), (std::vector<int32_t>{1, 3}));
    // Every document still counts as a hit.
    EXPECT_EQ(top.value().total_hits.value, 4);
}

TEST(SortTest, SecondaryFieldBreaksTheTie) {
    const IndexSearcher searcher = make_searcher();
    std::vector<SortField> fields{SortField::create("author", SortFieldType::kString).value(),
                                  SortField::with_reverse("price", SortFieldType::kInt, true).value()};
    auto sort = Sort::with_fields(std::move(fields));
    ASSERT_TRUE(sort.has_value());
    auto top = searcher.search(AllDocsQuery(), sort.value(), 10);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    // a/10, a/10, b/30, c/20
    EXPECT_EQ(hit_docs(top.value()), (std::vector<int32_t>{1, 3, 0, 2}));
    EXPECT_EQ(top.value().score_docs.size(), 4);
    EXPECT_EQ(top.value().score_docs[2].fields.size(), 2);
}

TEST(SortTest, TopDocsCanSkipTheFirstHits) {
    const IndexSearcher searcher = make_searcher();
    auto sort = Sort::with_fields({SortField::create("price", SortFieldType::kInt).value()});
    ASSERT_TRUE(sort.has_value());
    auto collector = TopFieldCollector::create(sort.value(), 4);
    ASSERT_TRUE(collector.has_value()) << collector.error().to_string();
    auto collected = searcher.search_with_collector(AllDocsQuery(), *collector.value());
    ASSERT_TRUE(collected.has_value()) << collected.error().to_string();

    auto top = collector.value()->top_docs(1);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    EXPECT_EQ(hit_docs(top.value()), (std::vector<int32_t>{3, 2, 0}));
    EXPECT_EQ(top.value().total_hits.value, 4);
    EXPECT_EQ(collector.value()->total_hits(), 4);
}

} // namespace

TEST(SortTest, SortsByTheSmallestSortedNumericValue) {
    // sizes: doc0 {10,40}, doc1 {5}, doc2 {}, doc3 {30,20}; a document without a
    // value sorts as 0.
    auto field = SortField::with_selector("sizes", SortFieldType::kSortedNumeric, SortFieldSelector::kMin);
    ASSERT_TRUE(field.has_value()) << field.error().to_string();
    auto sort = Sort::with_fields({field.value()});
    ASSERT_TRUE(sort.has_value());

    const IndexSearcher searcher = make_searcher();
    auto top = searcher.search(AllDocsQuery(), sort.value(), 10);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    const auto docs = hit_docs(top.value());
    ASSERT_EQ(docs.size(), 4U);
    EXPECT_EQ(docs[0], 2);
    EXPECT_EQ(docs[1], 1);
    EXPECT_EQ(docs[2], 0);
    EXPECT_EQ(docs[3], 3);
}

TEST(SortTest, SortsByTheLargestSortedNumericValue) {
    auto field = SortField::with_selector("sizes", SortFieldType::kSortedNumeric, SortFieldSelector::kMax);
    ASSERT_TRUE(field.has_value());
    auto sort = Sort::with_fields({field.value()});
    ASSERT_TRUE(sort.has_value());

    const IndexSearcher searcher = make_searcher();
    auto top = searcher.search(AllDocsQuery(), sort.value(), 10);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    const auto docs = hit_docs(top.value());
    ASSERT_EQ(docs.size(), 4U);
    EXPECT_EQ(docs[0], 2); // 0
    EXPECT_EQ(docs[1], 1); // 5
    EXPECT_EQ(docs[2], 3); // 30
    EXPECT_EQ(docs[3], 0); // 40
}

TEST(SortTest, SortsByTheSmallestSortedNumericDoubleValue) {
    auto field = SortField::with_selector("ratio", SortFieldType::kSortedNumericDouble, SortFieldSelector::kMin);
    ASSERT_TRUE(field.has_value());
    auto sort = Sort::with_fields({field.value()});
    ASSERT_TRUE(sort.has_value());

    const IndexSearcher searcher = make_searcher();
    auto top = searcher.search(AllDocsQuery(), sort.value(), 10);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    const auto docs = hit_docs(top.value());
    ASSERT_EQ(docs.size(), 4U);
    EXPECT_EQ(docs[0], 2); // 0.0
    EXPECT_EQ(docs[1], 0); // 0.5
    EXPECT_EQ(docs[2], 3); // 1.0
    EXPECT_EQ(docs[3], 1); // 9.0
}

TEST(SortTest, SortsByTheSmallestSortedSetTerm) {
    auto field = SortField::with_selector("tags", SortFieldType::kSortedSet, SortFieldSelector::kMin);
    ASSERT_TRUE(field.has_value());
    auto sort = Sort::with_fields({field.value()});
    ASSERT_TRUE(sort.has_value());

    const IndexSearcher searcher = make_searcher();
    auto top = searcher.search(AllDocsQuery(), sort.value(), 10);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    const auto docs = hit_docs(top.value());
    ASSERT_EQ(docs.size(), 4U);
    EXPECT_EQ(docs[0], 2); // no value: the empty string
    EXPECT_EQ(docs[1], 0); // alpha
    EXPECT_EQ(docs[2], 3); // alpha
    EXPECT_EQ(docs[3], 1); // beta
}

TEST(SortTest, SortsByTheLargestSortedSetTerm) {
    auto field = SortField::with_reverse_selector("tags", SortFieldType::kSortedSet, SortFieldSelector::kMax, true);
    ASSERT_TRUE(field.has_value());
    auto sort = Sort::with_fields({field.value()});
    ASSERT_TRUE(sort.has_value());

    const IndexSearcher searcher = make_searcher();
    auto top = searcher.search(AllDocsQuery(), sort.value(), 10);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    const auto docs = hit_docs(top.value());
    ASSERT_EQ(docs.size(), 4U);
    EXPECT_EQ(docs[0], 0); // gamma
    EXPECT_EQ(docs[1], 1); // beta
    EXPECT_EQ(docs[2], 3); // beta
    EXPECT_EQ(docs[3], 2); // no value
}

TEST(SortTest, SelectorRequiresAMultiValuedType) {
    auto field = SortField::with_selector("price", SortFieldType::kLong, SortFieldSelector::kMax);
    EXPECT_FALSE(field.has_value());
    EXPECT_EQ(field.error().kind(), ErrorKind::kIllegalArgument);
    auto no_field = SortField::with_selector(std::nullopt, SortFieldType::kSortedNumeric, SortFieldSelector::kMax);
    EXPECT_FALSE(no_field.has_value());
}

TEST(SortTest, MultiValuedSortFieldsIncludeTheSelectorInTheirText) {
    auto min = SortField::with_selector("sizes", SortFieldType::kSortedNumeric, SortFieldSelector::kMin);
    auto max = SortField::with_selector("sizes", SortFieldType::kSortedNumeric, SortFieldSelector::kMax);
    ASSERT_TRUE(min.has_value());
    ASSERT_TRUE(max.has_value());
    EXPECT_EQ(min.value().to_string().value(), "<sorted_numeric: \"sizes\">(min)");
    EXPECT_EQ(max.value().to_string().value(), "<sorted_numeric: \"sizes\">(max)");
    EXPECT_FALSE(min.value().equals(max.value()));
}
// ---------------------------------------------------------------------------
// paging (search_after) over a sort
// ---------------------------------------------------------------------------

namespace {

/// Walks every page of `sort` and returns the documents in order; a page that
/// comes back empty ends the walk, so a truncating page shows up as a short
/// result rather than a hang.
std::vector<int32_t> pages_of(const IndexSearcher &searcher, const Sort &sort, int32_t page_size,
                             int32_t max_pages = 8) {
    std::vector<int32_t> docs;
    std::optional<FieldDoc> after;
    for (int32_t page = 0; page < max_pages; ++page) {
        auto top = searcher.search_after(after.has_value() ? &after.value() : nullptr, AllDocsQuery(), sort, page_size);
        EXPECT_TRUE(top.has_value()) << (top.has_value() ? "" : top.error().to_string());
        if (!top.has_value() || top.value().score_docs.empty()) {
            break;
        }
        for (const FieldDoc &hit : top.value().score_docs) {
            docs.push_back(hit.doc);
        }
        after = top.value().score_docs.back();
    }
    return docs;
}

} // namespace

TEST(SortTest, PagesThroughANumericSort) {
    // price = 30, 10, 20, 10: ascending is 1, 3, 2, 0, and the two 10s are a tie
    // that the cursor has to break by document number.
    const IndexSearcher searcher = make_searcher();
    auto sort = Sort::with_fields({SortField::create("price", SortFieldType::kInt).value()});
    ASSERT_TRUE(sort.has_value());
    EXPECT_EQ(pages_of(searcher, sort.value(), 2), (std::vector<int32_t>{1, 3, 2, 0}));
    EXPECT_EQ(pages_of(searcher, sort.value(), 3), (std::vector<int32_t>{1, 3, 2, 0}));
    EXPECT_EQ(pages_of(searcher, sort.value(), 1), (std::vector<int32_t>{1, 3, 2, 0}));
}

TEST(SortTest, PagesThroughAReversedSort) {
    // Descending: 0 (30), 2 (20), 1 (10), 3 (10).
    const IndexSearcher searcher = make_searcher();
    auto sort = Sort::with_fields({SortField::with_reverse("price", SortFieldType::kInt, true).value()});
    ASSERT_TRUE(sort.has_value());
    EXPECT_EQ(pages_of(searcher, sort.value(), 2), (std::vector<int32_t>{0, 2, 1, 3}));
}

TEST(SortTest, PagesThroughATermSort) {
    // author = b, a, c, a: ascending is 1, 3, 0, 2.
    const IndexSearcher searcher = make_searcher();
    auto sort = Sort::with_fields({SortField::create("author", SortFieldType::kString).value()});
    ASSERT_TRUE(sort.has_value());
    EXPECT_EQ(pages_of(searcher, sort.value(), 3), (std::vector<int32_t>{1, 3, 0, 2}));
}

TEST(SortTest, PagesThroughARelevanceSort) {
    // The scores are 10, 20, 30, 40, so the order is 3, 2, 1, 0.
    const IndexSearcher searcher = make_searcher();
    auto top = searcher.search_after(nullptr, AllDocsQuery(), Sort::relevance().value(), 2);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    ASSERT_EQ(top.value().score_docs.size(), 2U);
    EXPECT_EQ(top.value().score_docs[0].doc, 3);
    EXPECT_EQ(top.value().score_docs[1].doc, 2);
    EXPECT_FLOAT_EQ(top.value().score_docs[1].score, 30.0F);

    auto next = searcher.search_after(&top.value().score_docs.back(), AllDocsQuery(), Sort::relevance().value(), 2);
    ASSERT_TRUE(next.has_value()) << next.error().to_string();
    ASSERT_EQ(next.value().score_docs.size(), 2U);
    EXPECT_EQ(next.value().score_docs[0].doc, 1);
    EXPECT_EQ(next.value().score_docs[1].doc, 0);
}

TEST(SortTest, PagesThroughAnIndexOrderSort) {
    const IndexSearcher searcher = make_searcher();
    EXPECT_EQ(pages_of(searcher, Sort::index_order().value(), 2), (std::vector<int32_t>{0, 1, 2, 3}));
}

TEST(SortTest, PagesThroughAMultiFieldSort) {
    // author then reversed price: 1 (a/10), 3 (a/10), 0 (b/30), 2 (c/20).
    const IndexSearcher searcher = make_searcher();
    std::vector<SortField> fields{SortField::create("author", SortFieldType::kString).value(),
                                  SortField::with_reverse("price", SortFieldType::kInt, true).value()};
    auto sort = Sort::with_fields(std::move(fields));
    ASSERT_TRUE(sort.has_value());
    EXPECT_EQ(pages_of(searcher, sort.value(), 3), (std::vector<int32_t>{1, 3, 0, 2}));
}

TEST(SortTest, APageLargerThanTheHitsGivesEverythingAndThenNothing) {
    const IndexSearcher searcher = make_searcher();
    auto sort = Sort::with_fields({SortField::create("price", SortFieldType::kInt).value()});
    ASSERT_TRUE(sort.has_value());

    auto first = searcher.search_after(nullptr, AllDocsQuery(), sort.value(), 4);
    ASSERT_TRUE(first.has_value()) << first.error().to_string();
    EXPECT_EQ(hit_docs(first.value()), (std::vector<int32_t>{1, 3, 2, 0}));
    EXPECT_EQ(first.value().total_hits.value, 4);

    auto second = searcher.search_after(&first.value().score_docs.back(), AllDocsQuery(), sort.value(), 4);
    ASSERT_TRUE(second.has_value()) << second.error().to_string();
    EXPECT_TRUE(second.value().score_docs.empty());
    // The hits of the previous page are still matches, so they are counted.
    EXPECT_EQ(second.value().total_hits.value, 4);
}

TEST(SortTest, AnAfterCursorKeepsOnlyTheHitsStrictlyAfterIt) {
    // A cursor that is not one of the hits still positions the page: everything
    // with a price above 15 is kept, in order.
    const IndexSearcher searcher = make_searcher();
    auto sort = Sort::with_fields({SortField::create("price", SortFieldType::kInt).value()});
    ASSERT_TRUE(sort.has_value());
    const FieldDoc cursor = FieldDoc::create(0, 0.0F, {SortValue(int64_t{15})});
    auto top = searcher.search_after(&cursor, AllDocsQuery(), sort.value(), 10);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    EXPECT_EQ(hit_docs(top.value()), (std::vector<int32_t>{2, 0}));
}

TEST(SortTest, AnAfterCursorNeedsOneValuePerSortField) {
    const IndexSearcher searcher = make_searcher();
    std::vector<SortField> fields{SortField::create("author", SortFieldType::kString).value(),
                                  SortField::create("price", SortFieldType::kInt).value()};
    auto sort = Sort::with_fields(std::move(fields));
    ASSERT_TRUE(sort.has_value());
    const FieldDoc cursor = FieldDoc::create(1, 0.0F, {SortValue(std::string("a"))});
    auto top = searcher.search_after(&cursor, AllDocsQuery(), sort.value(), 2);
    ASSERT_FALSE(top.has_value());
    EXPECT_EQ(top.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(SortTest, AnAfterCursorNeedsValuesOfTheSortedType) {
    const IndexSearcher searcher = make_searcher();
    auto sort = Sort::with_fields({SortField::create("price", SortFieldType::kInt).value()});
    ASSERT_TRUE(sort.has_value());
    const FieldDoc cursor = FieldDoc::create(1, 0.0F, {SortValue(std::string("10"))});
    auto top = searcher.search_after(&cursor, AllDocsQuery(), sort.value(), 2);
    ASSERT_FALSE(top.has_value());
    EXPECT_EQ(top.error().kind(), ErrorKind::kIllegalState);
}

TEST(SortTest, AnAfterCursorPastTheLastDocumentIsRejected) {
    const IndexSearcher searcher = make_searcher();
    auto sort = Sort::with_fields({SortField::create("price", SortFieldType::kInt).value()});
    ASSERT_TRUE(sort.has_value());
    const FieldDoc cursor = FieldDoc::create(4, 0.0F, {SortValue(int64_t{10})});
    auto top = searcher.search_after(&cursor, AllDocsQuery(), sort.value(), 2);
    ASSERT_FALSE(top.has_value());
    EXPECT_EQ(top.error().kind(), ErrorKind::kIllegalArgument);
}
