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

// End-to-end acceptance test: build an index from documents, reopen it from disk,
// and query it with the whole search stack.  Nothing here pokes at internals — it
// only uses the public API a caller of the library would use.

module;

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.e2e_index_search;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kId = "id";
constexpr std::string_view kBody = "body";
constexpr std::string_view kPrice = "price";
constexpr std::string_view kBrand = "brand";
constexpr std::string_view kStock = "stock";

FieldType text_type(bool store_term_vectors = false) {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqsAndPositions).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    if (store_term_vectors) {
        EXPECT_TRUE(type.set_store_term_vectors(true).has_value());
        EXPECT_TRUE(type.set_store_term_vector_positions(true).has_value());
    }
    return type;
}

FieldType keyword_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::Docs).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

/// One document of the little catalogue the test builds.
struct Product {
    std::string id;
    std::string body;
    int64_t price{0};
    std::string brand;
    int32_t stock{0};
};

Document make_product(const Product &product) {
    Document doc;
    doc.add(std::make_unique<Field>(std::string(kId), FieldValue{product.id}, keyword_type()));
    doc.add(std::make_unique<Field>(std::string(kBody), FieldValue{product.body}, text_type(true)));

    auto price = NumericDocValuesField::create(std::string(kPrice), product.price);
    EXPECT_TRUE(price.has_value()) << (price ? "" : price.error().to_string());
    doc.add(std::make_unique<Field>(std::move(price.value())));

    // A keyword field carries both: postings for the term range queries and doc
    // values for sorting, which is how Lucene models a keyword field.
    doc.add(std::make_unique<Field>(std::string(kBrand), FieldValue{product.brand}, keyword_type()));
    auto brand = SortedDocValuesField::create(std::string(kBrand), product.brand);
    EXPECT_TRUE(brand.has_value()) << (brand ? "" : brand.error().to_string());
    doc.add(std::make_unique<Field>(std::move(brand.value())));

    // A point for the numeric range queries, next to the doc values field.
    const int32_t point[1] = {product.stock};
    auto stock = IntPoint::create(std::string(kStock), std::span<const int32_t>(point, 1));
    EXPECT_TRUE(stock.has_value()) << (stock ? "" : stock.error().to_string());
    doc.add(std::make_unique<Field>(std::move(stock.value())));

    // A sorted numeric doc values field, queried through its range query.
    auto rating = SortedNumericDocValuesField::create("rating", product.price / 10);
    EXPECT_TRUE(rating.has_value()) << (rating ? "" : rating.error().to_string());
    doc.add(std::make_unique<Field>(std::move(rating.value())));

    // A sorted set doc values field with two values per document.
    const std::vector<std::string> tag_values{product.brand, product.stock > 0 ? "in_stock" : "out_of_stock"};
    for (const std::string &tag : tag_values) {
        auto sorted_set = SortedSetDocValuesField::create("tags", tag);
        EXPECT_TRUE(sorted_set.has_value()) << (sorted_set ? "" : sorted_set.error().to_string());
        doc.add(std::make_unique<Field>(std::move(sorted_set.value())));
    }

    // A range field, written as binary doc values.
    const int32_t range_min[1] = {product.stock};
    const int32_t range_max[1] = {product.stock + 5};
    auto stock_range = IntRange::create("stock_range", std::span<const int32_t>(range_min, 1),
                                        std::span<const int32_t>(range_max, 1));
    EXPECT_TRUE(stock_range.has_value()) << (stock_range ? "" : stock_range.error().to_string());
    doc.add(std::make_unique<IntRange>(std::move(stock_range.value())));
    return doc;
}

/// The catalogue: five products, written in two commits so the index has two
/// segments before the merge.
std::vector<Product> catalogue() {
    return {
            {"1", "red apple fruit", 10, "acme", 5},
            {"2", "green apple fruit", 20, "acme", 0},
            {"3", "banana fruit", 30, "beta", 12},
            {"4", "cherry fruit", 40, "beta", 3},
            {"5", "red cherry fruit", 50, "gamma", 0},
    };
}

/// Builds the index; the writer is closed, so the files are all there is.
void build_index(Directory &directory, std::span<const Product> products) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    const size_t half = products.size() / 2;
    for (size_t i = 0; i < products.size(); ++i) {
        ASSERT_TRUE(writer.value()->add_document(make_product(products[i])).has_value());
        if (i + 1 == half) {
            // A second segment, so the search has to span leaves.
            auto committed = writer.value()->commit();
            ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
        }
    }
    auto committed = writer.value()->commit();
    ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    ASSERT_TRUE(writer.value()->close().has_value());
}

/// The `id` values of `hits`, in the order the search returned them.
std::vector<std::string> ids_of(IndexSearcher &searcher, const TopDocs &hits) {
    std::vector<std::string> ids;
    auto stored = searcher.reader().stored_fields();
    EXPECT_TRUE(stored.has_value());
    if (!stored.has_value()) {
        return ids;
    }
    for (const ScoreDoc &hit : hits.score_docs) {
        auto doc = stored.value()->document(hit.doc);
        EXPECT_TRUE(doc.has_value());
        if (!doc.has_value()) {
            continue;
        }
        auto values = doc.value().get_values(std::string(kId));
        EXPECT_TRUE(values.has_value());
        ids.push_back(values.has_value() && !values.value().empty() ? values.value().front() : std::string{});
    }
    return ids;
}

std::vector<std::string> sorted_ids(std::vector<std::string> ids) {
    std::ranges::sort(ids);
    return ids;
}

int32_t count_of(IndexSearcher &searcher, const Query &query) {
    auto count = searcher.count(query);
    EXPECT_TRUE(count.has_value()) << (count ? "" : count.error().to_string());
    return count.value_or(-1);
}

} // namespace

TEST(EndToEndTest, BuildsAnIndexAndQueriesItAfterReopeningFromDisk) {
    ByteBuffersDirectory directory;
    const std::vector<Product> products = catalogue();
    build_index(directory, products);

    // A fresh reader over the committed files is what a search server starts with.
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->num_docs().value(), 5);
    IndexSearcher searcher(reader.value());

    // ---- every document is there ----
    auto all = searcher.search(MatchAllDocsQuery(), 10);
    ASSERT_TRUE(all.has_value()) << (all ? "" : all.error().to_string());
    EXPECT_EQ(all.value().total_hits.value, 5);
    EXPECT_EQ(sorted_ids(ids_of(searcher, all.value())), (std::vector<std::string>{"1", "2", "3", "4", "5"}));

    // ---- term and boolean queries ----
    EXPECT_EQ(count_of(searcher, TermQuery(Term::from_text(std::string(kBody), "apple"))), 2);
    EXPECT_EQ(count_of(searcher, TermQuery(Term::from_text(std::string(kBody), "missing"))), 0);

    BooleanQuery::Builder builder;
    ASSERT_TRUE(builder.add(std::make_shared<TermQuery>(Term::from_text(std::string(kBody), "fruit")),
                            Occur::Must)
                        .has_value());
    ASSERT_TRUE(builder.add(std::make_shared<TermQuery>(Term::from_text(std::string(kBody), "banana")),
                            Occur::Must)
                        .has_value());
    auto must = builder.build();
    ASSERT_TRUE(must.has_value()) << (must ? "" : must.error().to_string());
    EXPECT_EQ(count_of(searcher, must.value()), 1);

    BooleanQuery::Builder not_builder;
    ASSERT_TRUE(not_builder.add(std::make_shared<TermQuery>(Term::from_text(std::string(kBody), "cherry")),
                                Occur::Should)
                            .has_value());
    ASSERT_TRUE(not_builder.add(std::make_shared<TermQuery>(Term::from_text(std::string(kBody), "apple")),
                                Occur::MustNot)
                            .has_value());
    auto must_not = not_builder.build();
    ASSERT_TRUE(must_not.has_value());
    // Both cherry documents survive: neither contains "apple".
    EXPECT_EQ(count_of(searcher, must_not.value()), 2);

    // ---- multi-term queries ----
    EXPECT_EQ(count_of(searcher, PrefixQuery(std::string(kBody), "che")), 2);
    auto wildcard = WildcardQuery::create(Term::from_text(std::string(kBody), "app*"));
    ASSERT_TRUE(wildcard.has_value()) << (wildcard ? "" : wildcard.error().to_string());
    EXPECT_EQ(count_of(searcher, wildcard.value()), 2);
    auto fuzzy = FuzzyQuery::with_max_edits(Term::from_text(std::string(kBody), "aple"), 1);
    ASSERT_TRUE(fuzzy.has_value()) << (fuzzy ? "" : fuzzy.error().to_string());
    EXPECT_EQ(count_of(searcher, fuzzy.value()), 2);

    // ---- phrase query: positions are in the index ----
    auto phrase = PhraseQuery::create(std::string(kBody),
                                      {Term::from_text(std::string(kBody), "red"),
                                       Term::from_text(std::string(kBody), "apple")});
    ASSERT_TRUE(phrase.has_value()) << (phrase ? "" : phrase.error().to_string());
    EXPECT_EQ(count_of(searcher, phrase.value()), 1);

    // ---- term range on a keyword field ----
    auto brands = TermRangeQuery::create_string_range(std::string(kBrand), "acme", "beta", true, true);
    ASSERT_TRUE(brands.has_value()) << (brands ? "" : brands.error().to_string());
    EXPECT_EQ(count_of(searcher, brands.value()), 4);

    // ---- points range ----
    auto in_stock = IntPoint::new_range_query(std::string(kStock), 1, 100);
    ASSERT_TRUE(in_stock.has_value()) << (in_stock ? "" : in_stock.error().to_string());
    EXPECT_EQ(count_of(searcher, *in_stock.value()), 3);

    // ---- sorted numeric doc values range ----
    auto ratings = SortedNumericDocValuesField::new_slow_range_query("rating", 2, 4);
    ASSERT_TRUE(ratings.has_value()) << (ratings ? "" : ratings.error().to_string());
    EXPECT_EQ(count_of(searcher, ratings.value()), 3);

    // ---- sorted set doc values range and exact ----
    auto tags = SortedSetDocValuesField::new_slow_range_query("tags", BytesRef::from_string("acme"),
                                                             BytesRef::from_string("beta"), true, true);
    ASSERT_TRUE(tags.has_value()) << (tags ? "" : tags.error().to_string());
    EXPECT_EQ(count_of(searcher, tags.value()), 4);
    auto in_stock_tag = SortedSetDocValuesField::new_slow_exact_query("tags", BytesRef::from_string("in_stock"));
    ASSERT_TRUE(in_stock_tag.has_value()) << (in_stock_tag ? "" : in_stock_tag.error().to_string());
    EXPECT_EQ(count_of(searcher, in_stock_tag.value()), 3);

    // ---- range field: the ranges that intersect [1, 4] ----
    const int32_t query_min[1] = {1};
    const int32_t query_max[1] = {4};
    auto intersects = IntRange::new_intersects_query("stock_range", std::span<const int32_t>(query_min, 1),
                                                      std::span<const int32_t>(query_max, 1));
    ASSERT_TRUE(intersects.has_value()) << (intersects ? "" : intersects.error().to_string());
    // Stock ranges are [5,10], [0,5], [12,17], [3,8] and [0,5]: three touch [1,4].
    EXPECT_EQ(count_of(searcher, intersects.value()), 3);
    auto within = IntRange::new_within_query("stock_range", std::span<const int32_t>(query_min, 1),
                                             std::span<const int32_t>(query_max, 1));
    ASSERT_TRUE(within.has_value()) << (within ? "" : within.error().to_string());
    EXPECT_EQ(count_of(searcher, within.value()), 0);

    // ---- field exists ----
    EXPECT_EQ(count_of(searcher, FieldExistsQuery(std::string(kBrand))), 5);

    // ---- sorted search on a doc values field ----
    auto by_price = SortField::with_reverse(std::string(kPrice), SortFieldType::kLong, false);
    ASSERT_TRUE(by_price.has_value()) << (by_price ? "" : by_price.error().to_string());
    auto sort = Sort::with_fields({by_price.value()});
    ASSERT_TRUE(sort.has_value());
    auto sorted = searcher.search(MatchAllDocsQuery(), sort.value(), 10);
    ASSERT_TRUE(sorted.has_value()) << (sorted ? "" : sorted.error().to_string());
    ASSERT_EQ(sorted.value().score_docs.size(), 5U);
    EXPECT_EQ(sorted.value().total_hits.value, 5);
    std::vector<int64_t> prices;
    for (const FieldDoc &hit : sorted.value().score_docs) {
        ASSERT_EQ(hit.fields.size(), 1U);
        prices.push_back(std::get<int64_t>(hit.fields.front()));
    }
    EXPECT_EQ(prices, (std::vector<int64_t>{10, 20, 30, 40, 50}));

    // ---- pagination with search_after ----
    auto page1 = searcher.search_after_score(nullptr, MatchAllDocsQuery(), 2);
    ASSERT_TRUE(page1.has_value()) << (page1 ? "" : page1.error().to_string());
    ASSERT_EQ(page1.value().score_docs.size(), 2U);
    ScoreDoc cursor = page1.value().score_docs.back();
    auto page2 = searcher.search_after_score(&cursor, MatchAllDocsQuery(), 2);
    ASSERT_TRUE(page2.has_value());
    ASSERT_EQ(page2.value().score_docs.size(), 2U);
    EXPECT_GT(page2.value().score_docs.front().doc, cursor.doc);

    // ---- counts through a collector manager ----
    SynonymQuery::Builder synonyms;
    ASSERT_TRUE(synonyms.add_term(Term::from_text(std::string(kBody), "apple")).has_value());
    ASSERT_TRUE(synonyms.add_term(Term::from_text(std::string(kBody), "cherry")).has_value());
    EXPECT_EQ(count_of(searcher, synonyms.build()), 4);

    // ---- explanation of a real hit ----
    auto explained = searcher.explain(TermQuery(Term::from_text(std::string(kBody), "apple")),
                                      all.value().score_docs.front().doc);
    ASSERT_TRUE(explained.has_value()) << (explained ? "" : explained.error().to_string());
    EXPECT_FALSE(explained.value().to_string().empty());
}

TEST(EndToEndTest, UpdatesDeletesAndMergesStayConsistent) {
    ByteBuffersDirectory directory;
    const std::vector<Product> products = catalogue();
    build_index(directory, products);

    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    // Replace document 3 and delete document 4.
    ASSERT_TRUE(writer.value()
                        ->update_document(Term::from_text(std::string(kId), "3"),
                                          make_product({"3", "banana fruit bread", 35, "beta", 7}))
                        .has_value());
    ASSERT_TRUE(writer.value()->delete_documents(Term::from_text(std::string(kId), "4")).has_value());
    // And move a doc value in place.
    ASSERT_TRUE(writer.value()->update_numeric_doc_value(Term::from_text(std::string(kId), "1"), std::string(kPrice),
                                                       99)
                        .has_value());
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    EXPECT_EQ(reader.value()->num_docs().value(), 4);
    IndexSearcher searcher(reader.value());

    // The new version of document 3 is searchable, the old one is gone.
    EXPECT_EQ(count_of(searcher, TermQuery(Term::from_text(std::string(kBody), "bread"))), 1);
    EXPECT_EQ(count_of(searcher, TermQuery(Term::from_text(std::string(kId), "4"))), 0);

    // The in-place doc values update is visible.
    auto by_price = SortField::with_reverse(std::string(kPrice), SortFieldType::kLong, false);
    auto sort = Sort::with_fields({by_price.value()});
    auto sorted = searcher.search(TermQuery(Term::from_text(std::string(kBody), "fruit")), sort.value(), 10);
    ASSERT_TRUE(sorted.has_value()) << (sorted ? "" : sorted.error().to_string());
    std::vector<int64_t> prices;
    for (const FieldDoc &hit : sorted.value().score_docs) {
        prices.push_back(std::get<int64_t>(hit.fields.front()));
    }
    // Document 4 is gone, document 1 moved to 99: 20, 35, 50, 99.
    EXPECT_EQ(prices, (std::vector<int64_t>{20, 35, 50, 99}));

    // Merging down to one segment keeps everything.
    ASSERT_TRUE(writer.value()->force_merge(1).has_value());
    EXPECT_EQ(writer.value()->segment_count(), 1U);
    ASSERT_TRUE(writer.value()->close().has_value());

    auto merged = DirectoryReader::open(directory);
    ASSERT_TRUE(merged.has_value());
    EXPECT_EQ(merged.value()->num_docs().value(), 4);
    IndexSearcher merged_searcher(merged.value());
    EXPECT_EQ(count_of(merged_searcher, MatchAllDocsQuery()), 4);

    // A full index check over the result: terms, postings, norms, doc values,
    // stored fields, term vectors and points have to agree with each other.
    CheckIndex check(directory);
    auto checked = check.check();
    ASSERT_TRUE(checked.has_value()) << (checked ? "" : checked.error().to_string());
    EXPECT_TRUE(checked.value().clean()) << checked.value().to_string();
    EXPECT_EQ(checked.value().total_docs(), 4);
}

TEST(EndToEndTest, TheNearRealTimeReaderAndSearcherManagerSeeTheSameIndex) {
    ByteBuffersDirectory directory;
    const std::vector<Product> products = catalogue();
    build_index(directory, products);

    // A manager over the committed index.
    auto manager = SearcherManager::open(directory);
    ASSERT_TRUE(manager.has_value()) << (manager ? "" : manager.error().to_string());
    auto first = manager.value()->acquire();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(count_of(*first, MatchAllDocsQuery()), 5);

    // New documents arrive through a writer.
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer.value()
                        ->add_document(make_product({"6", "durian fruit", 60, "gamma", 2}))
                        .has_value());
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }

    // The old searcher still sees the old commit; a refresh picks up the new one.
    EXPECT_EQ(count_of(*first, MatchAllDocsQuery()), 5);
    auto refreshed = manager.value()->maybe_refresh();
    ASSERT_TRUE(refreshed.has_value());
    EXPECT_TRUE(refreshed.value());
    auto second = manager.value()->acquire();
    EXPECT_EQ(count_of(*second, MatchAllDocsQuery()), 6);

    // The near-real-time reader of the writer sees them too, without a commit.
    ASSERT_TRUE(writer.value()
                        ->add_document(make_product({"7", "elderberry fruit", 70, "gamma", 1}))
                        .has_value());
    auto nrt = writer.value()->get_reader();
    ASSERT_TRUE(nrt.has_value()) << (nrt ? "" : nrt.error().to_string());
    IndexSearcher nrt_searcher(nrt.value());
    EXPECT_EQ(count_of(nrt_searcher, MatchAllDocsQuery()), 7);

    // The committed index is still at six documents until the writer commits.
    auto reopened = DirectoryReader::open(directory);
    ASSERT_TRUE(reopened.has_value());
    EXPECT_EQ(reopened.value()->num_docs().value(), 6);
}

TEST(EndToEndTest, ATimeoutStopsALongSearch) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value());
    for (int32_t i = 0; i < 2000; ++i) {
        ASSERT_TRUE(writer.value()
                            ->add_document(make_product({std::to_string(i), "filler fruit", i, "bulk", i}))
                            .has_value());
    }
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }
    ASSERT_TRUE(writer.value()->close().has_value());

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    class Expired final : public QueryTimeout {
    public:
        [[nodiscard]] bool should_exit() const override { return true; }
    };
    Expired timeout;
    auto stopped = searcher.search(MatchAllDocsQuery(), 10, timeout);
    ASSERT_FALSE(stopped.has_value());
    EXPECT_EQ(stopped.error().kind(), ErrorKind::kTimeExceeded);

    // Without a timeout the same search returns its page.
    auto fine = searcher.search(MatchAllDocsQuery(), 10);
    ASSERT_TRUE(fine.has_value());
    EXPECT_EQ(fine.value().total_hits.value, 2000);
}

TEST(EndToEndTest, TheSameIndexWorksOnARealFileSystem) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_e2e_fs";
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);

    const std::vector<Product> products = catalogue();
    {
        auto directory = FSDirectory::open(path);
        ASSERT_TRUE(directory.has_value()) << (directory ? "" : directory.error().to_string());
        build_index(*directory.value(), products);
        ASSERT_TRUE(directory.value()->sync_metadata().has_value());
    }

    // Reopen from disk in a second directory handle: no state survives from the write.
    auto directory = FSDirectory::open(path);
    ASSERT_TRUE(directory.has_value());
    auto reader = DirectoryReader::open(*directory.value());
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->num_docs().value(), 5);
    IndexSearcher searcher(reader.value());
    EXPECT_EQ(count_of(searcher, TermQuery(Term::from_text(std::string(kBody), "apple"))), 2);

    auto by_price = SortField::with_reverse(std::string(kPrice), SortFieldType::kLong, true);
    auto sort = Sort::with_fields({by_price.value()});
    auto sorted = searcher.search(TermQuery(Term::from_text(std::string(kBody), "fruit")), sort.value(), 10);
    ASSERT_TRUE(sorted.has_value()) << (sorted ? "" : sorted.error().to_string());
    std::vector<int64_t> prices;
    for (const FieldDoc &hit : sorted.value().score_docs) {
        prices.push_back(std::get<int64_t>(hit.fields.front()));
    }
    EXPECT_EQ(prices, (std::vector<int64_t>{50, 40, 30, 20, 10}));

    // The commit is durable: a full check passes on the files that were written.
    CheckIndex check(*directory.value());
    auto checked = check.check();
    ASSERT_TRUE(checked.has_value()) << (checked ? "" : checked.error().to_string());
    EXPECT_TRUE(checked.value().clean()) << checked.value().to_string();

    std::filesystem::remove_all(path, ignored);
}

namespace {

/// The `id` stored field of one hit.
std::string id_of(IndexSearcher &searcher, int32_t doc) {
    auto stored = searcher.reader().stored_fields();
    EXPECT_TRUE(stored.has_value());
    if (!stored.has_value()) {
        return {};
    }
    auto document = stored.value()->document(doc);
    EXPECT_TRUE(document.has_value());
    if (!document.has_value()) {
        return {};
    }
    auto values = document.value().get_values(std::string(kId));
    EXPECT_TRUE(values.has_value());
    return values.has_value() && !values.value().empty() ? values.value().front() : std::string{};
}

/// Walks every page of `sort` and returns the `id`s in order.
std::vector<std::string> paged_ids(IndexSearcher &searcher, const Sort &sort, int32_t page_size,
                                  int32_t expected_total_hits) {
    std::vector<std::string> ids;
    std::optional<FieldDoc> after;
    const int32_t max_pages = expected_total_hits / std::max(page_size, 1) + 2;
    for (int32_t page = 0; page < max_pages; ++page) {
        auto top = searcher.search_after(after.has_value() ? &after.value() : nullptr, MatchAllDocsQuery(), sort,
                                         page_size);
        EXPECT_TRUE(top.has_value()) << (top.has_value() ? "" : top.error().to_string());
        if (!top.has_value() || top.value().score_docs.empty()) {
            break;
        }
        // Every match counts on every page, the previous ones included.
        EXPECT_EQ(top.value().total_hits.value, expected_total_hits);
        for (const FieldDoc &hit : top.value().score_docs) {
            ids.push_back(id_of(searcher, hit.doc));
        }
        after = top.value().score_docs.back();
    }
    return ids;
}

} // namespace

TEST(EndToEndTest, PagesThroughASortedSearchAcrossSegments) {
    ByteBuffersDirectory directory;
    const std::vector<Product> products = catalogue(); // ids 1..5, prices 10..50, two segments
    build_index(directory, products);

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    IndexSearcher searcher(reader.value());

    auto ascending = SortField::with_reverse(std::string(kPrice), SortFieldType::kLong, false);
    ASSERT_TRUE(ascending.has_value());
    auto by_price = Sort::with_fields({ascending.value()});
    ASSERT_TRUE(by_price.has_value());
    EXPECT_EQ(paged_ids(searcher, by_price.value(), 2, 5), (std::vector<std::string>{"1", "2", "3", "4", "5"}));
    EXPECT_EQ(paged_ids(searcher, by_price.value(), 1, 5), (std::vector<std::string>{"1", "2", "3", "4", "5"}));

    auto descending = SortField::with_reverse(std::string(kPrice), SortFieldType::kLong, true);
    ASSERT_TRUE(descending.has_value());
    auto by_price_desc = Sort::with_fields({descending.value()});
    ASSERT_TRUE(by_price_desc.has_value());
    EXPECT_EQ(paged_ids(searcher, by_price_desc.value(), 2, 5), (std::vector<std::string>{"5", "4", "3", "2", "1"}));

    // A brand sort has only three distinct values, so the cursor has to break
    // the ties by document number to keep every hit.
    auto by_brand = Sort::with_fields({SortField::create(std::string(kBrand), SortFieldType::kString).value(),
                                       ascending.value()});
    ASSERT_TRUE(by_brand.has_value());
    // acme/10, acme/20, beta/30, beta/40, gamma/50
    EXPECT_EQ(paged_ids(searcher, by_brand.value(), 2, 5), (std::vector<std::string>{"1", "2", "3", "4", "5"}));
}

TEST(EndToEndTest, PagingKeepsEveryHitOfADeeplyTiedSort) {
    // Only two distinct prices, so each page ends inside one long tie and the
    // cursor's document number is the only thing keeping hits from being
    // dropped or repeated between pages.
    std::vector<Product> products;
    products.reserve(51);
    for (int32_t i = 0; i < 51; ++i) {
        products.push_back({std::to_string(i), "filler fruit", i % 2 == 0 ? 10 : 20, "acme", i % 3});
    }

    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    for (size_t i = 0; i < products.size(); ++i) {
        ASSERT_TRUE(writer.value()->add_document(make_product(products[i])).has_value());
        if ((i + 1) % 17 == 0) {
            ASSERT_TRUE(writer.value()->commit().has_value());
        }
    }
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    IndexSearcher searcher(reader.value());

    auto by_price = Sort::with_fields({SortField::create(std::string(kPrice), SortFieldType::kLong).value()});
    ASSERT_TRUE(by_price.has_value());
    const std::vector<std::string> ids = paged_ids(searcher, by_price.value(), 5, 51);

    std::vector<std::string> expected;
    expected.reserve(51);
    for (int32_t i = 0; i < 51; i += 2) {
        expected.push_back(std::to_string(i)); // price 10
    }
    for (int32_t i = 1; i < 51; i += 2) {
        expected.push_back(std::to_string(i)); // price 20
    }
    EXPECT_EQ(ids, expected);
}
