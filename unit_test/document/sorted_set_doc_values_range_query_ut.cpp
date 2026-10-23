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
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.sorted_set_doc_values_range_query;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kField = "tags";

std::optional<BytesRef> bound(std::string_view text) {
    return std::optional<BytesRef>(BytesRef::from_string(text));
}

/// A sorted set doc values cursor over one ordinal list per document; the
/// dictionary is given explicitly and `lookup_ord` reads it back as text.
class FakeSortedSet final : public SortedSetDocValues {
public:
    FakeSortedSet(std::vector<std::vector<int64_t>> ords, std::vector<std::string> dictionary)
            : ords_(std::move(ords)), dictionary_(std::move(dictionary)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override { return advance(doc_ + 1); }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        doc_ = static_cast<size_t>(target) < ords_.size() ? target : NO_MORE_DOCS;
        index_ = 0;
        return doc_;
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(ords_.size()); }

    [[nodiscard]] Result<int64_t> next_ord() override {
        const auto &ords = ords_[static_cast<size_t>(doc_)];
        if (index_ >= ords.size()) {
            return NO_MORE_ORDS;
        }
        return ords[index_++];
    }

    [[nodiscard]] Result<int32_t> doc_value_count() override {
        return static_cast<int32_t>(ords_[static_cast<size_t>(doc_)].size());
    }

    [[nodiscard]] Result<BytesRef> lookup_ord(int64_t ord) override {
        if (ord < 0 || static_cast<size_t>(ord) >= dictionary_.size()) {
            return err(LuceneError::illegal_argument("ordinal out of range"));
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
    size_t index_{0};
};

/// A skipper with the whole field as both level 0 and level 1.
class FakeFieldSkipper final : public DocValuesSkipper {
public:
    FakeFieldSkipper(int32_t doc_count, int64_t min_value, int64_t max_value, int32_t value_count)
            : doc_count_(doc_count), min_value_(min_value), max_value_(max_value), value_count_(value_count) {}

    [[nodiscard]] VoidResult advance(int32_t /*target*/) override { return {}; }
    [[nodiscard]] size_t num_levels() const override { return 1; }
    [[nodiscard]] int32_t min_doc_id(size_t /*level*/) const override { return 0; }
    [[nodiscard]] int32_t max_doc_id(size_t /*level*/) const override { return doc_count_ - 1; }
    [[nodiscard]] int64_t min_value(size_t /*level*/) const override { return min_value_; }
    [[nodiscard]] int64_t max_value(size_t /*level*/) const override { return max_value_; }
    [[nodiscard]] int32_t doc_count(size_t /*level*/) const override { return value_count_; }
    [[nodiscard]] int64_t global_min_value() const override { return min_value_; }
    [[nodiscard]] int64_t global_max_value() const override { return max_value_; }
    [[nodiscard]] int32_t global_doc_count() const override { return value_count_; }

private:
    int32_t doc_count_;
    int64_t min_value_;
    int64_t max_value_;
    int32_t value_count_;
};

class RangeLeafReader final : public LeafReader {
public:
    RangeLeafReader(std::vector<std::vector<int64_t>> ords, std::vector<std::string> dictionary,
                    std::shared_ptr<DocValuesSkipper> skipper = nullptr)
            : max_doc_(static_cast<int32_t>(ords.size())),
              ords_(std::move(ords)),
              dictionary_(std::move(dictionary)),
              skipper_(std::move(skipper)) {
        FieldInfo info;
        info.number = 0;
        info.name = std::string(kField);
        info.doc_values_type = DocValuesType::SortedSet;
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
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric_doc_values(std::string_view /*field*/) override {
        return std::unique_ptr<NumericDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> get_binary_doc_values(std::string_view /*field*/) override {
        return std::unique_ptr<BinaryDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted_doc_values(std::string_view /*field*/) override {
        return std::unique_ptr<SortedDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>>
    get_sorted_numeric_doc_values(std::string_view /*field*/) override {
        return std::unique_ptr<SortedNumericDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<SortedSetDocValues>>
    get_sorted_set_doc_values(std::string_view field) override {
        if (field != kField) {
            return std::unique_ptr<SortedSetDocValues>{};
        }
        return std::unique_ptr<SortedSetDocValues>(std::make_unique<FakeSortedSet>(ords_, dictionary_));
    }
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norm_values(std::string_view /*field*/) override {
        return std::unique_ptr<NumericDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<DocValuesSkipper>> get_doc_values_skipper(
            std::string_view field) override {
        if (field != kField || skipper_ == nullptr) {
            return std::unique_ptr<DocValuesSkipper>{};
        }
        auto skipper = skipper_;
        auto copied = std::make_unique<FakeFieldSkipper>(*static_cast<FakeFieldSkipper *>(skipper.get()));
        return std::unique_ptr<DocValuesSkipper>(std::move(copied));
    }

    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return &field_infos_; }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return static_cast<const Bits *>(nullptr); }

private:
    int32_t max_doc_;
    std::vector<std::vector<int64_t>> ords_;
    std::vector<std::string> dictionary_;
    std::shared_ptr<DocValuesSkipper> skipper_;
    FieldInfos field_infos_{FieldInfos::none()};
};

std::vector<int32_t> range_docs(const SortedSetDocValuesRangeQuery &query, IndexSearcher &searcher) {
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
        if (scorer.value()->matches().value()) {
            docs.push_back(doc.value());
        }
    }
    return docs;
}

/// `a` .. `e` with one document per interesting combination.
std::shared_ptr<RangeLeafReader> dictionary_reader(int32_t docs = 5) {
    std::vector<std::vector<int64_t>> ords{{0, 2}, {3}, {1}, {4}, {2, 4}};
    EXPECT_EQ(static_cast<int32_t>(ords.size()), docs);
    return std::make_shared<RangeLeafReader>(std::move(ords), std::vector<std::string>{"a", "b", "c", "d", "e"});
}

} // namespace

TEST(SortedSetDocValuesRangeQueryTest, MatchesDocumentsWithAnyOrdinalInTheRange) {
    auto reader = dictionary_reader();
    IndexSearcher searcher(reader);

    auto query = SortedSetDocValuesRangeQuery::create(std::string(kField), bound("b"), bound("d"), true, true);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    // Documents 0 (c) and 4 (c) have an ordinal inside [b, d]; document 3 only
    // has `e` which is above the range.
    EXPECT_EQ(range_docs(query.value(), searcher), (std::vector<int32_t>{0, 1, 2, 4}));
}

TEST(SortedSetDocValuesRangeQueryTest, ExcludesTheBoundsWhenTheyAreExclusive) {
    auto reader = dictionary_reader();
    IndexSearcher searcher(reader);

    auto query = SortedSetDocValuesRangeQuery::create(std::string(kField), bound("b"), bound("d"), false, false);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    // Only `c` remains.
    EXPECT_EQ(range_docs(query.value(), searcher), (std::vector<int32_t>{0, 4}));
}

TEST(SortedSetDocValuesRangeQueryTest, BoundsMissingFromTheDictionaryUseTheirInsertionPoint) {
    // Dictionary `b`, `d`, `f`: `c` inserts before `d` and `e` after `d`, so
    // [c TO e] selects exactly the ordinal of `d`.
    auto reader = std::make_shared<RangeLeafReader>(std::vector<std::vector<int64_t>>{{0, 1}, {0}, {1, 2}},
            std::vector<std::string>{"b", "d", "f"});
    IndexSearcher searcher(reader);

    auto query = SortedSetDocValuesRangeQuery::create(std::string(kField), bound("c"), bound("e"), true, true);
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(range_docs(query.value(), searcher), (std::vector<int32_t>{0, 2}));

    // A range between two existing terms selects nothing.
    auto gap = SortedSetDocValuesRangeQuery::create(std::string(kField), bound("b"), bound("c"), false, false);
    ASSERT_TRUE(gap.has_value()) << gap.error().to_string();
    EXPECT_TRUE(range_docs(gap.value(), searcher).empty());
}

TEST(SortedSetDocValuesRangeQueryTest, OpenEndpointsAreUnbounded) {
    auto reader = dictionary_reader();
    IndexSearcher searcher(reader);

    auto lower_only = SortedSetDocValuesRangeQuery::create(std::string(kField), bound("d"), std::nullopt, true, true);
    ASSERT_TRUE(lower_only.has_value()) << lower_only.error().to_string();
    EXPECT_EQ(range_docs(lower_only.value(), searcher), (std::vector<int32_t>{1, 3, 4}));

    auto upper_only = SortedSetDocValuesRangeQuery::create(std::string(kField), std::nullopt, bound("b"), true, true);
    ASSERT_TRUE(upper_only.has_value()) << upper_only.error().to_string();
    EXPECT_EQ(range_docs(upper_only.value(), searcher), (std::vector<int32_t>{0, 2}));
}

TEST(SortedSetDocValuesRangeQueryTest, RewritesTheFullyOpenRangeToFieldExists) {
    IndexSearcher searcher(dictionary_reader());

    auto full = SortedSetDocValuesRangeQuery::create(std::string(kField), std::nullopt, std::nullopt, true, true);
    ASSERT_TRUE(full.has_value());
    // An open endpoint cannot be exclusive.
    EXPECT_FALSE(full.value().lower_inclusive());
    EXPECT_FALSE(full.value().upper_inclusive());

    auto rewritten = full.value().rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value()) << rewritten.error().to_string();
    ASSERT_TRUE(rewritten.value() != nullptr);
    EXPECT_TRUE(dynamic_cast<const FieldExistsQuery *>(rewritten.value().get()) != nullptr);

    auto closed = SortedSetDocValuesRangeQuery::create(std::string(kField), bound("b"), std::nullopt, true, true);
    ASSERT_TRUE(closed.has_value());
    auto unchanged = closed.value().rewrite(searcher);
    ASSERT_TRUE(unchanged.has_value());
    EXPECT_TRUE(unchanged.value() == nullptr);
}

TEST(SortedSetDocValuesRangeQueryTest, DescribesAndComparesItself) {
    auto first = SortedSetDocValuesRangeQuery::create(std::string(kField), bound("b"), bound("d"), true, false);
    ASSERT_TRUE(first.has_value());
    auto same = SortedSetDocValuesRangeQuery::create(std::string(kField), bound("b"), bound("d"), true, false);
    ASSERT_TRUE(same.has_value());
    auto other = SortedSetDocValuesRangeQuery::create(std::string(kField), bound("b"), bound("d"), true, true);
    ASSERT_TRUE(other.has_value());

    EXPECT_TRUE(first.value().equals(same.value()));
    EXPECT_FALSE(first.value().equals(other.value()));
    EXPECT_EQ(first.value().hash(), same.value().hash());

    auto rendered = first.value().to_string(std::string(kField));
    ASSERT_TRUE(rendered.has_value());
    EXPECT_EQ(rendered.value(), "[b TO d}");
    auto qualified = first.value().to_string("other");
    ASSERT_TRUE(qualified.has_value());
    EXPECT_EQ(qualified.value(), std::string(kField) + ":[b TO d}");

    auto open = SortedSetDocValuesRangeQuery::create(std::string(kField), std::nullopt, std::nullopt, true, true);
    ASSERT_TRUE(open.has_value());
    auto open_text = open.value().to_string(std::string(kField));
    ASSERT_TRUE(open_text.has_value());
    EXPECT_EQ(open_text.value(), "{* TO *}");
}

TEST(SortedSetDocValuesRangeQueryTest, MissingFieldMatchesNothing) {
    IndexSearcher searcher(dictionary_reader());

    auto query = SortedSetDocValuesRangeQuery::create("other", bound("b"), bound("d"), true, true);
    ASSERT_TRUE(query.has_value());
    EXPECT_TRUE(range_docs(query.value(), searcher).empty());
}

TEST(SortedSetDocValuesRangeQueryTest, ExplainsMatchingAndNonMatchingDocuments) {
    auto reader = dictionary_reader();
    IndexSearcher searcher(reader);

    auto query = SortedSetDocValuesRangeQuery::create(std::string(kField), bound("b"), bound("d"), true, true);
    ASSERT_TRUE(query.has_value());
    auto weight = query.value().create_weight(searcher, ScoreMode::CompleteNoScores, 1.0F);
    ASSERT_TRUE(weight.has_value());
    auto leaves = searcher.leaves();
    ASSERT_TRUE(leaves.has_value());

    auto matched = weight.value()->explain(leaves.value()[0], 2, searcher);
    ASSERT_TRUE(matched.has_value()) << matched.error().to_string();
    EXPECT_TRUE(matched.value().is_match());

    auto missed = weight.value()->explain(leaves.value()[0], 3, searcher);
    ASSERT_TRUE(missed.has_value()) << missed.error().to_string();
    EXPECT_FALSE(missed.value().is_match());
}

TEST(SortedSetDocValuesRangeQueryTest, UsesTheSkipperToShortCircuit) {
    // The whole field maps to ordinals [0, 4]; a range above it matches nothing
    // and a range covering it matches every document.
    auto out_of_range = std::make_shared<RangeLeafReader>(std::vector<std::vector<int64_t>>{{0}, {1}},
            std::vector<std::string>{"a", "b"}, std::make_shared<FakeFieldSkipper>(2, 0, 1, 2));
    IndexSearcher none_searcher(out_of_range);
    auto none = SortedSetDocValuesRangeQuery::create(std::string(kField), bound("c"), bound("e"), true, true);
    ASSERT_TRUE(none.has_value());
    EXPECT_TRUE(range_docs(none.value(), none_searcher).empty());

    auto covered = std::make_shared<RangeLeafReader>(std::vector<std::vector<int64_t>>{{0}, {1}},
            std::vector<std::string>{"a", "b"}, std::make_shared<FakeFieldSkipper>(2, 0, 1, 2));
    IndexSearcher all_searcher(covered);
    auto all = SortedSetDocValuesRangeQuery::create(std::string(kField), bound("a"), bound("b"), true, true);
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(range_docs(all.value(), all_searcher), (std::vector<int32_t>{0, 1}));
}
