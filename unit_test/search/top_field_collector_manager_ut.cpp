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

// Sorting through a collector manager: the merge of the per-slice results has
// to answer the same hits as a single collector over the whole index, both for
// the first page and for a search-after page.

module;

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.top_field_collector_manager;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kBody = "body";
constexpr std::string_view kNum = "num";

/// Builds `segments * per_segment` documents; `num` is the document number and
/// `price` cycles through only three values, so the sort has ties in it.
void build_index(Directory &directory, int32_t segments, int32_t per_segment) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());

    FieldType type;
    ASSERT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    int32_t doc = 0;
    for (int32_t segment = 0; segment < segments; ++segment) {
        for (int32_t i = 0; i < per_segment; ++i, ++doc) {
            Document document;
            document.add(std::make_unique<Field>(std::string(kBody), FieldValue{std::string("alpha")}, type));
            document.add(std::make_unique<Field>(std::move(NumericDocValuesField::create(std::string(kNum), doc).value())));
            ASSERT_TRUE(writer.value()->add_document(std::move(document)).has_value());
        }
        ASSERT_TRUE(writer.value()->commit().has_value());
    }
    ASSERT_TRUE(writer.value()->close().has_value());
}

std::vector<int32_t> docs_of(const TopFieldDocs &top) {
    std::vector<int32_t> docs;
    for (const FieldDoc &hit : top.score_docs) {
        docs.push_back(hit.doc);
    }
    return docs;
}

std::vector<int64_t> values_of(const TopFieldDocs &top) {
    std::vector<int64_t> values;
    for (const FieldDoc &hit : top.score_docs) {
        values.push_back(std::get<int64_t>(hit.fields.front()));
    }
    return values;
}

Sort sort_by_num() {
    auto field = SortField::create(std::string(kNum), SortFieldType::kLong);
    EXPECT_TRUE(field.has_value());
    auto sort = Sort::with_fields({field.value()});
    EXPECT_TRUE(sort.has_value());
    return sort.value();
}

/// The id of the document whose sort value the merge has to answer first.
FieldDoc cursor_for(int32_t doc, int64_t value) {
    return FieldDoc::create(doc, 0.0F, {SortValue(value)});
}

} // namespace

// ---------------------------------------------------------------------------
// the merge of sorted shards
// ---------------------------------------------------------------------------

TEST(MergeTopFieldDocsTest, MergesTheShardsByTheirSortValues) {
    const Sort sort = sort_by_num();
    const std::vector<TopFieldDocs> shards{
            TopFieldDocs(TotalHits::create(2, TotalHits::Relation::EqualTo),
                         {FieldDoc::create(0, 0.0F, {SortValue(int64_t{10})}),
                          FieldDoc::create(2, 0.0F, {SortValue(int64_t{30})})},
                         sort),
            TopFieldDocs(TotalHits::create(2, TotalHits::Relation::EqualTo),
                         {FieldDoc::create(1, 0.0F, {SortValue(int64_t{20})}),
                          FieldDoc::create(3, 0.0F, {SortValue(int64_t{40})})},
                         sort),
    };
    auto merged = merge_top_field_docs(sort, 10, shards);
    ASSERT_TRUE(merged.has_value()) << merged.error().to_string();
    EXPECT_EQ(docs_of(merged.value()), (std::vector<int32_t>{0, 1, 2, 3}));
    EXPECT_EQ(values_of(merged.value()), (std::vector<int64_t>{10, 20, 30, 40}));
    // The merge reports the total of every shard, not the number of hits it kept.
    EXPECT_EQ(merged.value().total_hits.value, 4);
}

TEST(MergeTopFieldDocsTest, SkipsTheFirstHitsAndKeepsTheWindow) {
    const Sort sort = sort_by_num();
    const std::vector<TopFieldDocs> shards{
            TopFieldDocs(TotalHits::create(2, TotalHits::Relation::EqualTo),
                         {FieldDoc::create(0, 0.0F, {SortValue(int64_t{10})}),
                          FieldDoc::create(2, 0.0F, {SortValue(int64_t{30})})},
                         sort),
            TopFieldDocs(TotalHits::create(2, TotalHits::Relation::EqualTo),
                         {FieldDoc::create(1, 0.0F, {SortValue(int64_t{20})}),
                          FieldDoc::create(3, 0.0F, {SortValue(int64_t{40})})},
                         sort),
    };
    auto merged = merge_top_field_docs_with_start(sort, 1, 2, shards);
    ASSERT_TRUE(merged.has_value()) << merged.error().to_string();
    EXPECT_EQ(docs_of(merged.value()), (std::vector<int32_t>{1, 2}));
    EXPECT_EQ(values_of(merged.value()), (std::vector<int64_t>{20, 30}));
}

TEST(MergeTopFieldDocsTest, MergesAReversedSort) {
    auto field = SortField::with_reverse(std::string(kNum), SortFieldType::kLong, true);
    ASSERT_TRUE(field.has_value());
    auto sort = Sort::with_fields({field.value()});
    ASSERT_TRUE(sort.has_value());
    const std::vector<TopFieldDocs> shards{
            TopFieldDocs(TotalHits::create(2, TotalHits::Relation::EqualTo),
                         {FieldDoc::create(2, 0.0F, {SortValue(int64_t{30})}),
                          FieldDoc::create(0, 0.0F, {SortValue(int64_t{10})})},
                         sort.value()),
            TopFieldDocs(TotalHits::create(2, TotalHits::Relation::EqualTo),
                         {FieldDoc::create(3, 0.0F, {SortValue(int64_t{40})}),
                          FieldDoc::create(1, 0.0F, {SortValue(int64_t{20})})},
                         sort.value()),
    };
    auto merged = merge_top_field_docs(sort.value(), 10, shards);
    ASSERT_TRUE(merged.has_value()) << merged.error().to_string();
    EXPECT_EQ(values_of(merged.value()), (std::vector<int64_t>{40, 30, 20, 10}));
}

TEST(MergeTopFieldDocsTest, RejectsShardsWithoutSortValues) {
    const Sort sort = sort_by_num();
    std::vector<TopFieldDocs> shards{
            TopFieldDocs(TotalHits::create(1, TotalHits::Relation::EqualTo),
                         {FieldDoc::create(0, 0.0F, {})}, // a hit that carries no sort value
                         sort),
    };
    auto merged = merge_top_field_docs(sort, 10, shards);
    ASSERT_FALSE(merged.has_value());
    EXPECT_EQ(merged.error().kind(), ErrorKind::kIllegalArgument);
}

// ---------------------------------------------------------------------------
// the collector manager
// ---------------------------------------------------------------------------

TEST(TopFieldCollectorManagerTest, RejectsACursorWithoutSortValues) {
    const Sort sort = sort_by_num();
    auto manager = TopFieldCollectorManager::with_after(sort, 5, FieldDoc::create(0, 0.0F, {}));
    ASSERT_FALSE(manager.has_value());
    EXPECT_EQ(manager.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(TopFieldCollectorManagerTest, RejectsACursorOfTheWrongWidth) {
    const Sort sort = sort_by_num();
    auto manager = TopFieldCollectorManager::with_after(sort, 5, cursor_for(0, 10)); // one value for one field
    ASSERT_TRUE(manager.has_value()) << manager.error().to_string();

    auto two_fields = Sort::with_fields({SortField::create(std::string(kNum), SortFieldType::kLong).value(),
                                         SortField::create(std::string(kNum), SortFieldType::kLong).value()});
    ASSERT_TRUE(two_fields.has_value());
    auto wrong = TopFieldCollectorManager::with_after(two_fields.value(), 5, cursor_for(0, 10));
    ASSERT_FALSE(wrong.has_value());
    EXPECT_EQ(wrong.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(TopFieldCollectorManagerTest, RejectsANegativeNumberOfHits) {
    const Sort sort = sort_by_num();
    auto created = TopFieldCollectorManager::create(sort, -1);
    ASSERT_FALSE(created.has_value());
    EXPECT_EQ(created.error().kind(), ErrorKind::kIllegalArgument);
    auto paged = TopFieldCollectorManager::with_after(sort, -1, cursor_for(0, 10));
    ASSERT_FALSE(paged.has_value());
    EXPECT_EQ(paged.error().kind(), ErrorKind::kIllegalArgument);
}

// ---------------------------------------------------------------------------
// the searcher on top of the manager
// ---------------------------------------------------------------------------

TEST(TopFieldCollectorManagerTest, ParallelSortedSearchMatchesASingleCollector) {
    ByteBuffersDirectory directory;
    build_index(directory, 6, 10); // six segments, sixty documents, three distinct sort values
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());

    const Sort sort = sort_by_num();
    const IndexSearcher serial(reader.value());
    auto parallel = IndexSearcher::with_executor(reader.value(), nullptr, 4);
    ASSERT_TRUE(parallel.has_value()) << parallel.error().to_string();

    // The reference: one collector over the whole index.
    auto collector = TopFieldCollector::create(sort, 10);
    ASSERT_TRUE(collector.has_value()) << collector.error().to_string();
    ASSERT_TRUE(serial.search_with_collector(MatchAllDocsQuery(), *collector.value()).has_value());
    auto expected = collector.value()->top_docs();
    ASSERT_TRUE(expected.has_value()) << expected.error().to_string();
    ASSERT_EQ(expected.value().score_docs.size(), 10U);

    // The manager path of the serial searcher (one collector per slice) and of
    // the parallel one answer the same hits with the same sort values.
    auto managed = serial.search(MatchAllDocsQuery(), sort, 10);
    ASSERT_TRUE(managed.has_value()) << managed.error().to_string();
    EXPECT_EQ(docs_of(managed.value()), docs_of(expected.value()));
    EXPECT_EQ(values_of(managed.value()), values_of(expected.value()));
    EXPECT_EQ(managed.value().total_hits.value, 60);

    auto threaded = parallel.value()->search(MatchAllDocsQuery(), sort, 10);
    ASSERT_TRUE(threaded.has_value()) << threaded.error().to_string();
    EXPECT_EQ(docs_of(threaded.value()), docs_of(expected.value()));
    EXPECT_EQ(values_of(threaded.value()), values_of(expected.value()));
    EXPECT_EQ(threaded.value().total_hits.value, 60);
}

TEST(TopFieldCollectorManagerTest, ParallelPagingMatchesTheSerialOne) {
    ByteBuffersDirectory directory;
    build_index(directory, 6, 10);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());

    const Sort sort = sort_by_num();
    const IndexSearcher serial(reader.value());
    auto parallel = IndexSearcher::with_executor(reader.value(), nullptr, 4);
    ASSERT_TRUE(parallel.has_value());

    std::vector<int32_t> serial_docs;
    std::vector<int32_t> parallel_docs;
    std::optional<FieldDoc> serial_after;
    std::optional<FieldDoc> parallel_after;
    for (int32_t page = 0; page < 12; ++page) { // ten pages, plus the empty one
        auto serial_page = serial.search_after(serial_after.has_value() ? &serial_after.value() : nullptr,
                                               MatchAllDocsQuery(), sort, 7);
        ASSERT_TRUE(serial_page.has_value()) << serial_page.error().to_string();
        auto parallel_page = parallel.value()->search_after(parallel_after.has_value() ? &parallel_after.value() : nullptr,
                                                            MatchAllDocsQuery(), sort, 7);
        ASSERT_TRUE(parallel_page.has_value()) << parallel_page.error().to_string();
        ASSERT_EQ(parallel_page.value().score_docs.size(), serial_page.value().score_docs.size());
        if (serial_page.value().score_docs.empty()) {
            break;
        }
        for (const FieldDoc &hit : serial_page.value().score_docs) {
            serial_docs.push_back(hit.doc);
        }
        for (const FieldDoc &hit : parallel_page.value().score_docs) {
            parallel_docs.push_back(hit.doc);
        }
        serial_after = serial_page.value().score_docs.back();
        parallel_after = parallel_page.value().score_docs.back();
    }
    EXPECT_EQ(parallel_docs, serial_docs);
    EXPECT_EQ(serial_docs.size(), 60U);
}

TEST(TopFieldCollectorManagerTest, ARelevanceSortGoesThroughTheSamePath) {
    ByteBuffersDirectory directory;
    build_index(directory, 6, 10);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());

    auto parallel = IndexSearcher::with_executor(reader.value(), nullptr, 3);
    ASSERT_TRUE(parallel.has_value());

    auto top = parallel.value()->search(MatchAllDocsQuery(), Sort::relevance().value(), 5);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    ASSERT_EQ(top.value().score_docs.size(), 5U);
    EXPECT_EQ(top.value().total_hits.value, 60);
    // Every document scores the same, so the order is the document order and the
    // sort values are the scores.
    EXPECT_EQ(docs_of(top.value()), (std::vector<int32_t>{0, 1, 2, 3, 4}));
    for (const FieldDoc &hit : top.value().score_docs) {
        EXPECT_FLOAT_EQ(hit.score, std::get<float>(hit.fields.front()));
    }
}

// ---------------------------------------------------------------------------
// the total-hits threshold and the index-sort early termination
// ---------------------------------------------------------------------------

namespace {

/// Builds `count` index-sorted documents whose `num` runs from 0 upwards; they
/// are added in reverse order so that the flush has to sort them.
void build_sorted_index(Directory &directory, int32_t count, const Sort &index_sort) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_index_sort(index_sort);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());

    FieldType type;
    ASSERT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    for (int32_t i = count - 1; i >= 0; --i) {
        Document document;
        document.add(std::make_unique<Field>(std::string(kBody), FieldValue{std::string("alpha")}, type));
        document.add(std::make_unique<Field>(
                std::move(NumericDocValuesField::create(std::string(kNum), static_cast<int64_t>(i)).value())));
        ASSERT_TRUE(writer.value()->add_document(std::move(document)).has_value());
    }
    ASSERT_TRUE(writer.value()->close().has_value());
}

SortField num_field(bool reverse) {
    auto field = SortField::with_reverse(std::string(kNum), SortFieldType::kLong, reverse);
    EXPECT_TRUE(field.has_value());
    return field.value();
}

} // namespace

TEST(CanEarlyTerminateTest, ASortByDocumentNumberAlwaysAllowsIt) {
    EXPECT_TRUE(can_early_terminate(Sort::index_order().value(), nullptr));
    // A relevance sort does not: the documents are not visited in score order.
    EXPECT_FALSE(can_early_terminate(Sort::relevance().value(), nullptr));
}

TEST(CanEarlyTerminateTest, APrefixOfTheIndexSortAllowsIt) {
    auto index_sort = Sort::with_fields({num_field(false),
                                         SortField::with_reverse(std::string(kBody), SortFieldType::kString, true)
                                                 .value()});
    ASSERT_TRUE(index_sort.has_value());
    auto prefix = Sort::with_fields({num_field(false)});
    ASSERT_TRUE(prefix.has_value());
    auto longer = Sort::with_fields({num_field(false),
                                     SortField::with_reverse(std::string(kBody), SortFieldType::kString, true).value(),
                                     SortField::create(std::string("other"), SortFieldType::kLong).value()});
    ASSERT_TRUE(longer.has_value());
    auto reversed = Sort::with_fields({num_field(true)});
    ASSERT_TRUE(reversed.has_value());

    EXPECT_TRUE(can_early_terminate(prefix.value(), &index_sort.value()));
    // A full index sort is a prefix of itself.
    EXPECT_TRUE(can_early_terminate(index_sort.value(), &index_sort.value()));
    // The sort may be shorter than the index sort, never longer.
    EXPECT_FALSE(can_early_terminate(longer.value(), &index_sort.value()));
    // The direction and the type have to match.
    EXPECT_FALSE(can_early_terminate(reversed.value(), &index_sort.value()));
    EXPECT_FALSE(can_early_terminate(prefix.value(), nullptr));
}

TEST(TopFieldCollectorThresholdTest, StopsEarlyWhenTheSortMatchesTheIndexSort) {
    ByteBuffersDirectory directory;
    auto index_sort = Sort::with_fields({num_field(false)});
    ASSERT_TRUE(index_sort.has_value());
    build_sorted_index(directory, 20, index_sort.value());

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    const IndexSearcher searcher(reader.value());

    auto collector = TopFieldCollector::create(index_sort.value(), 2, nullptr, 5);
    ASSERT_TRUE(collector.has_value()) << collector.error().to_string();
    // The early termination is reported as a terminated collection, which the
    // searcher swallows: the hits collected so far are the answer.
    ASSERT_TRUE(searcher.search_with_collector(MatchAllDocsQuery(), *collector.value()).has_value());

    auto top = collector.value()->top_docs();
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    EXPECT_EQ(values_of(top.value()), (std::vector<int64_t>{0, 1}));
    // The two lowest hits of two hits in hand: collection stopped once the count
    // was past the threshold, so the total is a lower bound.
    EXPECT_EQ(top.value().total_hits.relation, TotalHits::Relation::GreaterThanOrEqualTo);
    EXPECT_EQ(top.value().total_hits.value, 6);
}

TEST(TopFieldCollectorThresholdTest, CountsEveryHitWithoutAnIndexSort) {
    ByteBuffersDirectory directory;
    build_index(directory, 2, 10); // twenty documents, no index sort

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    const IndexSearcher searcher(reader.value());

    const Sort sort = sort_by_num();
    auto collector = TopFieldCollector::create(sort, 2, nullptr, 5);
    ASSERT_TRUE(collector.has_value()) << collector.error().to_string();
    ASSERT_TRUE(searcher.search_with_collector(MatchAllDocsQuery(), *collector.value()).has_value());

    auto top = collector.value()->top_docs();
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    // The documents do not arrive in sort order, so collection cannot stop; the
    // threshold only downgrades the relation.
    EXPECT_EQ(top.value().total_hits.value, 20);
    EXPECT_EQ(top.value().total_hits.relation, TotalHits::Relation::GreaterThanOrEqualTo);
    EXPECT_EQ(values_of(top.value()), (std::vector<int64_t>{0, 1}));
}

TEST(TopFieldCollectorThresholdTest, CountsExactlyWithTheDefaultThreshold) {
    ByteBuffersDirectory directory;
    auto index_sort = Sort::with_fields({num_field(false)});
    ASSERT_TRUE(index_sort.has_value());
    build_sorted_index(directory, 20, index_sort.value());

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    const IndexSearcher searcher(reader.value());

    // No threshold means every hit is counted, so no early termination either.
    auto collector = TopFieldCollector::create(index_sort.value(), 2);
    ASSERT_TRUE(collector.has_value()) << collector.error().to_string();
    ASSERT_TRUE(searcher.search_with_collector(MatchAllDocsQuery(), *collector.value()).has_value());

    auto top = collector.value()->top_docs();
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    EXPECT_EQ(top.value().total_hits.value, 20);
    EXPECT_EQ(top.value().total_hits.relation, TotalHits::Relation::EqualTo);
    EXPECT_EQ(values_of(top.value()), (std::vector<int64_t>{0, 1}));
}

TEST(TopFieldCollectorThresholdTest, TheSearcherUsesTheThresholdOnAnIndexSortedIndex) {
    ByteBuffersDirectory directory;
    auto index_sort = Sort::with_fields({num_field(false)});
    ASSERT_TRUE(index_sort.has_value());
    build_sorted_index(directory, 20, index_sort.value());

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    const IndexSearcher searcher(reader.value());

    // Twenty hits stay below the searcher's threshold, so the count is exact.
    auto top = searcher.search(MatchAllDocsQuery(), index_sort.value(), 3);
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    EXPECT_EQ(top.value().total_hits.value, 20);
    EXPECT_EQ(top.value().total_hits.relation, TotalHits::Relation::EqualTo);
    EXPECT_EQ(values_of(top.value()), (std::vector<int64_t>{0, 1, 2}));
}

TEST(TopFieldCollectorThresholdTest, AFiniteThresholdKeepsTheCompetitiveHits) {
    // The documents are added in the reverse of their sort order and the index is
    // not sorted, so a threshold that dropped the comparison of the later
    // documents would lose the best hits.
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());
    FieldType type;
    ASSERT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    for (int32_t i = 19; i >= 0; --i) {
        Document document;
        document.add(std::make_unique<Field>(std::string(kBody), FieldValue{std::string("alpha")}, type));
        document.add(std::make_unique<Field>(
                std::move(NumericDocValuesField::create(std::string(kNum), static_cast<int64_t>(i)).value())));
        ASSERT_TRUE(writer.value()->add_document(std::move(document)).has_value());
    }
    ASSERT_TRUE(writer.value()->close().has_value());

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    const IndexSearcher searcher(reader.value());

    const Sort sort = sort_by_num();
    auto collector = TopFieldCollector::create(sort, 3, nullptr, 5);
    ASSERT_TRUE(collector.has_value()) << collector.error().to_string();
    ASSERT_TRUE(searcher.search_with_collector(MatchAllDocsQuery(), *collector.value()).has_value());

    auto top = collector.value()->top_docs();
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    EXPECT_EQ(values_of(top.value()), (std::vector<int64_t>{0, 1, 2}));
    // Every document is visited because the sort is not the index sort, so the
    // total is exact even though the relation is a lower bound.
    EXPECT_EQ(top.value().total_hits.value, 20);
    EXPECT_EQ(top.value().total_hits.relation, TotalHits::Relation::GreaterThanOrEqualTo);
}

TEST(TopFieldCollectorThresholdTest, ARelevanceSortSkipsHitsThatCannotWin) {
    // Document `i` holds "alpha" `i % 20 + 1` times, so the scores differ and the
    // five best documents are the ones with the highest frequency.
    ByteBuffersDirectory directory;
    {
        WhitespaceAnalyzer analyzer;
        IndexWriterConfig config(analyzer);
        auto writer = IndexWriter::open(directory, config);
        ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());
        FieldType type;
        ASSERT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
        for (int32_t doc = 0; doc < 200; ++doc) {
            Document document;
            for (int32_t occurrence = 0; occurrence <= doc % 20; ++occurrence) {
                document.add(std::make_unique<Field>(std::string(kBody), FieldValue{std::string("alpha")}, type));
            }
            ASSERT_TRUE(writer.value()->add_document(std::move(document)).has_value());
        }
        ASSERT_TRUE(writer.value()->close().has_value());
    }

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    const IndexSearcher searcher(reader.value());
    const TermQuery query(Term::from_text(std::string(kBody), "alpha"));

    // A threshold well below the number of hits lets the collector tell the scorer
    // which scores cannot win any more.
    auto accumulator = std::make_shared<MaxScoreAccumulator>();
    auto collector = TopFieldCollector::create(Sort::relevance().value(), 5, nullptr, /*total_hits_threshold=*/5,
                                               accumulator);
    ASSERT_TRUE(collector.has_value()) << collector.error().to_string();
    ASSERT_TRUE(searcher.search_with_collector(query, *collector.value()).has_value());
    auto top = collector.value()->top_docs();
    ASSERT_TRUE(top.has_value()) << top.error().to_string();

    // The oracle is the score-ordered search, which uses another collector.
    auto expected = searcher.search(query, 5);
    ASSERT_TRUE(expected.has_value()) << expected.error().to_string();
    ASSERT_EQ(expected.value().score_docs.size(), 5U);
    // Every document of the top five holds the term as often as the oracle says.
    ASSERT_EQ(top.value().score_docs.size(), 5U);
    for (size_t i = 0; i < expected.value().score_docs.size(); ++i) {
        EXPECT_FLOAT_EQ(top.value().score_docs[i].score, expected.value().score_docs[i].score) << i;
        EXPECT_EQ(top.value().score_docs[i].doc, expected.value().score_docs[i].doc) << i;
    }
    // The count became a lower bound once the threshold was crossed.
    EXPECT_EQ(top.value().total_hits.relation, TotalHits::Relation::GreaterThanOrEqualTo);
    EXPECT_GT(top.value().total_hits.value, 5U);
}
