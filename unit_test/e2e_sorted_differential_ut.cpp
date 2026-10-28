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

// Sorted searches against a brute-force scan: a random document set is sorted by
// hand, and the pages of `search_after` (single page, every page, several page
// sizes, a parallel searcher and an index-sorted index) have to name exactly the
// documents of that scan, in that order, without dropping or repeating one.

module;

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.e2e_sorted_differential;

import lucene_core;

using namespace lucene;

namespace {

constexpr int32_t DOC_COUNT = 200;
constexpr std::string_view K_TERM = "t3";

/// One source document, mirroring what the index stores.
struct SourceDoc {
    bool has_k_term{false};
    int64_t number{0};
    std::string tag;
};

/// A random document set and the index written from it.
struct Fixture {
    std::filesystem::path path;
    std::vector<SourceDoc> docs;

    Fixture(std::string_view name, uint32_t seed) {
        path = std::filesystem::temp_directory_path() / std::string(name);
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);

        std::mt19937 engine(seed);
        std::uniform_int_distribution<int64_t> number_pick(0, 60); // plenty of ties
        std::uniform_int_distribution<int32_t> tag_pick(0, 3);
        std::uniform_int_distribution<int32_t> term_pick(0, 1);
        for (int32_t id = 0; id < DOC_COUNT; ++id) {
            SourceDoc doc;
            doc.has_k_term = term_pick(engine) == 0;
            doc.number = number_pick(engine);
            doc.tag = "tag" + std::to_string(tag_pick(engine));
            docs.push_back(std::move(doc));
        }
    }

    ~Fixture() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    Fixture(const Fixture &) = delete;
    Fixture &operator=(const Fixture &) = delete;
};

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    return type;
}

FieldType stored_id_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::Docs).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

Document make_document(const SourceDoc &source, int32_t id) {
    Document doc;
    doc.add(std::make_unique<Field>("id", FieldValue{std::to_string(id)}, stored_id_type()));
    doc.add(std::make_unique<Field>("body", FieldValue{std::string("common")}, text_type()));
    if (source.has_k_term) {
        doc.add(std::make_unique<Field>("body", FieldValue{std::string(K_TERM)}, text_type()));
    }
    auto number = NumericDocValuesField::create("num", source.number);
    EXPECT_TRUE(number.has_value());
    doc.add(std::make_unique<Field>(std::move(number.value())));
    auto tag_values = SortedSetDocValuesField::create("tagdv", source.tag);
    EXPECT_TRUE(tag_values.has_value());
    doc.add(std::make_unique<Field>(std::move(tag_values.value())));
    return doc;
}

/// Writes every document into a fresh index; several segments, and index-sorted
/// by `num` when `index_sort` is given.  `deleted` names the ids to delete
/// afterwards, and `merge_to` forces the index down to that many segments.
void build(Fixture &fixture, const std::optional<Sort> &index_sort = std::nullopt,
           const std::vector<int32_t> &deleted = {}, int32_t merge_to = 0, bool no_merge = false) {
    auto directory = FSDirectory::open(fixture.path);
    ASSERT_TRUE(directory.has_value());
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_max_buffered_docs(37);
    if (no_merge) {
        // Without this the commit merges the segments and drops the deleted
        // documents, which is exactly what the other case tests.
        config.set_merge_policy(NoMergePolicy::create());
    }
    if (index_sort.has_value()) {
        config.set_index_sort(index_sort.value());
    }
    auto writer = IndexWriter::open(*directory.value(), config);
    ASSERT_TRUE(writer.has_value());
    for (int32_t id = 0; id < DOC_COUNT; ++id) {
        ASSERT_TRUE(writer.value()->add_document(make_document(fixture.docs[static_cast<size_t>(id)], id)).has_value());
    }
    for (const int32_t id : deleted) {
        ASSERT_TRUE(writer.value()->delete_documents(Term::from_text("id", std::to_string(id))).has_value());
    }
    if (merge_to > 0) {
        ASSERT_TRUE(writer.value()->force_merge(merge_to).has_value());
    }
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());
}

/// A run of sorted hits: the sort keys and the stored ids, in hit order.
struct SortedRun {
    std::vector<int64_t> numbers;
    std::vector<std::string> tags;
    std::vector<int32_t> ids;
};

int32_t stored_id(DirectoryReader &reader, int32_t doc) {
    auto stored = reader.stored_fields();
    EXPECT_TRUE(stored.has_value());
    if (!stored.has_value()) {
        return -1;
    }
    auto document = stored.value()->document(doc);
    EXPECT_TRUE(document.has_value());
    if (!document.has_value()) {
        return -1;
    }
    auto values = document.value().get_values("id");
    EXPECT_TRUE(values.has_value());
    if (!values.has_value() || values.value().empty()) {
        return -1;
    }
    return std::atoi(values.value().front().c_str());
}

/// Reads one page into `run`, with `sort_keys` telling how many sort values a hit
/// carries (one: a number, two: a tag then a number).
void collect(SortedRun &run, DirectoryReader &reader, const TopFieldDocs &top, bool with_tag) {
    for (const FieldDoc &hit : top.score_docs) {
        EXPECT_EQ(hit.fields.size(), with_tag ? 2U : 1U);
        if (with_tag) {
            run.tags.push_back(std::get<std::string>(hit.fields[0]));
            run.numbers.push_back(std::get<int64_t>(hit.fields[1]));
        } else {
            run.numbers.push_back(std::get<int64_t>(hit.fields[0]));
        }
        run.ids.push_back(stored_id(reader, hit.doc));
    }
}

SortedRun one_page(IndexSearcher &searcher, DirectoryReader &reader, const Query &query, const Sort &sort,
                   int32_t limit, bool with_tag) {
    SortedRun run;
    auto hits = searcher.search_after(nullptr, query, sort, limit);
    EXPECT_TRUE(hits.has_value()) << (hits.has_value() ? "" : hits.error().to_string());
    if (!hits.has_value()) {
        return run;
    }
    collect(run, reader, hits.value(), with_tag);
    return run;
}

SortedRun every_page(IndexSearcher &searcher, DirectoryReader &reader, const Query &query, const Sort &sort,
                     int32_t page_size, int32_t expected_total, bool with_tag) {
    SortedRun run;
    std::optional<FieldDoc> after;
    const int32_t max_pages = expected_total / std::max(page_size, 1) + 2;
    for (int32_t page = 0; page < max_pages; ++page) {
        auto hits = searcher.search_after(after.has_value() ? &after.value() : nullptr, query, sort, page_size);
        EXPECT_TRUE(hits.has_value()) << (hits.has_value() ? "" : hits.error().to_string());
        if (!hits.has_value() || hits.value().score_docs.empty()) {
            break;
        }
        collect(run, reader, hits.value(), with_tag);
        after = hits.value().score_docs.back();
    }
    return run;
}

/// The matching documents, from the source data.
std::vector<int32_t> matching_ids(const std::vector<SourceDoc> &docs, const std::function<bool(const SourceDoc &)> &keep) {
    std::vector<int32_t> ids;
    for (int32_t id = 0; id < static_cast<int32_t>(docs.size()); ++id) {
        if (keep(docs[static_cast<size_t>(id)])) {
            ids.push_back(id);
        }
    }
    return ids;
}

std::vector<int64_t> expected_numbers(const std::vector<SourceDoc> &docs, const std::vector<int32_t> &matching,
                                      bool descending) {
    std::vector<int64_t> numbers;
    numbers.reserve(matching.size());
    for (const int32_t id : matching) {
        numbers.push_back(docs[static_cast<size_t>(id)].number);
    }
    std::sort(numbers.begin(), numbers.end());
    if (descending) {
        std::reverse(numbers.begin(), numbers.end());
    }
    return numbers;
}

/// The numbers of the documents sorted by tag ascending and number descending.
std::vector<std::pair<std::string, int64_t>> expected_tag_then_number(const std::vector<SourceDoc> &docs,
                                                                      const std::vector<int32_t> &matching) {
    std::vector<std::pair<std::string, int64_t>> keys;
    keys.reserve(matching.size());
    for (const int32_t id : matching) {
        keys.emplace_back(docs[static_cast<size_t>(id)].tag, docs[static_cast<size_t>(id)].number);
    }
    std::sort(keys.begin(), keys.end(), [](const auto &left, const auto &right) {
        if (left.first != right.first) {
            return left.first < right.first;
        }
        return left.second > right.second;
    });
    return keys;
}

/// Asserts a run is ordered like `expected`, holds exactly that many documents and
/// returns none of them twice.
void expect_numbers(const SortedRun &run, const std::vector<int64_t> &expected, std::string_view label) {
    ASSERT_EQ(run.numbers.size(), expected.size()) << label;
    for (size_t i = 0; i < expected.size(); ++i) {
        ASSERT_EQ(run.numbers[i], expected[i]) << label << " at position " << i;
    }
    std::vector<int32_t> ids = run.ids;
    std::sort(ids.begin(), ids.end());
    ASSERT_EQ(std::unique(ids.begin(), ids.end()), ids.end()) << label << ": a document was returned twice";
}

} // namespace

TEST(SortedDifferentialTest, NumericSortsAgreeWithABruteForceScan) {
    uint32_t seed = 20261025;
    if (const char *configured = std::getenv("CLUCENE_DIFF_SEED"); configured != nullptr) {
        seed = static_cast<uint32_t>(std::strtoul(configured, nullptr, 10));
    }
    Fixture fixture("clucene_sorted_differential", seed);
    build(fixture);

    auto directory = FSDirectory::open(fixture.path);
    ASSERT_TRUE(directory.has_value());
    auto reader = DirectoryReader::open(*directory.value());
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    ASSERT_GT(reader.value()->leaves().value().size(), 1U) << "the index has to have several segments";

    IndexSearcher searcher(reader.value());
    auto parallel = IndexSearcher::with_executor(reader.value(), nullptr, 3);
    ASSERT_TRUE(parallel.has_value()) << parallel.error().to_string();

    auto num_asc = Sort::with_fields({SortField::with_reverse("num", SortFieldType::kLong, false).value()});
    ASSERT_TRUE(num_asc.has_value());
    auto num_desc = Sort::with_fields({SortField::with_reverse("num", SortFieldType::kLong, true).value()});
    ASSERT_TRUE(num_desc.has_value());

    const std::vector<std::pair<std::string, std::function<bool(const SourceDoc &)>>> filters{
            {"match all", [](const SourceDoc &) { return true; }},
            {"term t3", [](const SourceDoc &doc) { return doc.has_k_term; }},
    };
    const std::vector<std::pair<std::string, std::shared_ptr<Query>>> queries{
            {"match all", std::make_shared<MatchAllDocsQuery>()},
            {"term t3", std::make_shared<TermQuery>(Term::from_text("body", std::string(K_TERM)))},
    };

    for (size_t q = 0; q < queries.size(); ++q) {
        const std::vector<int32_t> matching = matching_ids(fixture.docs, filters[q].second);
        ASSERT_FALSE(matching.empty());
        for (const bool descending : {false, true}) {
            const Sort &sort = descending ? num_desc.value() : num_asc.value();
            const std::string label =
                    queries[q].first + (descending ? " / num desc" : " / num asc");
            const std::vector<int64_t> all = expected_numbers(fixture.docs, matching, descending);

            for (const int32_t limit : {1, 5, 10}) {
                const auto end = static_cast<size_t>(std::min<int32_t>(limit, static_cast<int32_t>(all.size())));
                expect_numbers(one_page(searcher, *reader.value(), *queries[q].second, sort, limit, false),
                               std::vector<int64_t>(all.begin(), all.begin() + end),
                               label + " limit " + std::to_string(limit));
            }
            for (const int32_t page_size : {1, 7, 50, DOC_COUNT}) {
                expect_numbers(every_page(searcher, *reader.value(), *queries[q].second, sort, page_size, DOC_COUNT,
                                          false),
                               all, label + " pages of " + std::to_string(page_size));
            }
            expect_numbers(every_page(*parallel.value(), *reader.value(), *queries[q].second, sort, 7, DOC_COUNT, false),
                           all, label + " pages of 7 (parallel)");
        }
    }

    // Two sort fields: the tag first, then the number in the opposite direction.
    auto tag_then_number = Sort::with_fields({SortField::create("tagdv", SortFieldType::kSortedSet).value(),
                                              SortField::with_reverse("num", SortFieldType::kLong, true).value()});
    ASSERT_TRUE(tag_then_number.has_value());
    const std::vector<int32_t> matching = matching_ids(fixture.docs, [](const SourceDoc &) { return true; });
    const auto expected = expected_tag_then_number(fixture.docs, matching);
    for (const int32_t page_size : {1, 7, 50}) {
        const SortedRun run = every_page(searcher, *reader.value(), MatchAllDocsQuery(), tag_then_number.value(),
                                         page_size, DOC_COUNT, true);
        ASSERT_EQ(run.tags.size(), expected.size());
        for (size_t i = 0; i < expected.size(); ++i) {
            ASSERT_EQ(run.tags[i], expected[i].first) << "tag at position " << i;
            ASSERT_EQ(run.numbers[i], expected[i].second) << "number at position " << i;
        }
    }
}

TEST(SortedDifferentialTest, PagingOnAnIndexSortedIndexAgreesWithABruteForceScan) {
    uint32_t seed = 20261025;
    if (const char *configured = std::getenv("CLUCENE_DIFF_SEED"); configured != nullptr) {
        seed = static_cast<uint32_t>(std::strtoul(configured, nullptr, 10));
    }
    auto index_sort = Sort::with_fields({SortField::with_reverse("num", SortFieldType::kLong, false).value()});
    ASSERT_TRUE(index_sort.has_value());
    Fixture fixture("clucene_sorted_differential_index", seed);
    build(fixture, index_sort.value());

    auto directory = FSDirectory::open(fixture.path);
    ASSERT_TRUE(directory.has_value());
    auto reader = DirectoryReader::open(*directory.value());
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    IndexSearcher searcher(reader.value());

    const std::vector<int32_t> matching = matching_ids(fixture.docs, [](const SourceDoc &doc) { return doc.has_k_term; });
    ASSERT_FALSE(matching.empty());
    const std::vector<int64_t> expected = expected_numbers(fixture.docs, matching, /*descending=*/false);
    const TermQuery query(Term::from_text("body", std::string(K_TERM)));

    for (const int32_t page_size : {1, 7, 50}) {
        expect_numbers(every_page(searcher, *reader.value(), query, index_sort.value(), page_size, DOC_COUNT, false),
                       expected, "index sorted, pages of " + std::to_string(page_size));
    }

    // The hits have to stay correct when the collection stops early: the sort is
    // the index sort, so collection may end at the first non-competitive document.
    auto collector = TopFieldCollector::create(index_sort.value(), 5, nullptr, /*total_hits_threshold=*/5);
    ASSERT_TRUE(collector.has_value()) << collector.error().to_string();
    ASSERT_TRUE(searcher.search_with_collector(query, *collector.value()).has_value());
    auto top = collector.value()->top_docs();
    ASSERT_TRUE(top.has_value()) << top.error().to_string();
    const auto end = static_cast<size_t>(std::min<size_t>(5, expected.size()));
    std::vector<int64_t> actual;
    for (const FieldDoc &hit : top.value().score_docs) {
        actual.push_back(std::get<int64_t>(hit.fields.front()));
    }
    EXPECT_EQ(actual, std::vector<int64_t>(expected.begin(), expected.begin() + end));
    if (matching.size() > 5) {
        // Collection stopped before the end of the matching documents, so the
        // count became a lower bound.
        EXPECT_EQ(top.value().total_hits.relation, TotalHits::Relation::GreaterThanOrEqualTo);
        EXPECT_LE(top.value().total_hits.value, matching.size());
    }
}

TEST(SortedDifferentialTest, PagingAgreesAfterDeletesAndAMerge) {
    uint32_t seed = 20261025;
    if (const char *configured = std::getenv("CLUCENE_DIFF_SEED"); configured != nullptr) {
        seed = static_cast<uint32_t>(std::strtoul(configured, nullptr, 10));
    }
    // Every third document is deleted: once with the deletions still live in the
    // segments, once after a merge dropped them (which renumbers the documents, so
    // the pages have to be found through the stored ids).
    std::vector<int32_t> deleted;
    for (int32_t id = 0; id < DOC_COUNT; id += 3) {
        deleted.push_back(id);
    }

    auto num_asc = Sort::with_fields({SortField::with_reverse("num", SortFieldType::kLong, false).value()});
    ASSERT_TRUE(num_asc.has_value());
    const TermQuery query(Term::from_text("body", std::string(K_TERM)));

    // `merge_to` = 0 keeps the deletions, 1 merges them away.
    const auto check = [&](Fixture &fixture, int32_t merge_to, std::string_view label) {
        std::vector<int32_t> live;
        std::vector<int32_t> live_matching;
        for (int32_t id = 0; id < DOC_COUNT; ++id) {
            if (std::find(deleted.begin(), deleted.end(), id) != deleted.end()) {
                continue;
            }
            live.push_back(id);
            if (fixture.docs[static_cast<size_t>(id)].has_k_term) {
                live_matching.push_back(id);
            }
        }
        ASSERT_EQ(live.size(), static_cast<size_t>(DOC_COUNT - static_cast<int32_t>(deleted.size())));

        auto directory = FSDirectory::open(fixture.path);
        ASSERT_TRUE(directory.has_value());
        auto reader = DirectoryReader::open(*directory.value());
        ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
        IndexSearcher searcher(reader.value());

        const std::vector<int64_t> all_numbers = expected_numbers(fixture.docs, live, /*descending=*/false);
        const std::vector<int64_t> matching_numbers = expected_numbers(fixture.docs, live_matching, /*descending=*/false);

        // The deleted documents must not come back, and paging must still tile the
        // result exactly once.
        for (const int32_t page_size : {1, 7, 50}) {
            expect_numbers(every_page(searcher, *reader.value(), MatchAllDocsQuery(), num_asc.value(), page_size,
                                      static_cast<int32_t>(live.size()), false),
                           all_numbers, std::string(label) + ", pages of " + std::to_string(page_size));
            expect_numbers(every_page(searcher, *reader.value(), query, num_asc.value(), page_size,
                                      static_cast<int32_t>(live_matching.size()), false),
                           matching_numbers, std::string(label) + " + term, pages of " + std::to_string(page_size));
        }

        // Counting, the sorted search and the source data have to agree.
        auto counted = searcher.count(query);
        ASSERT_TRUE(counted.has_value()) << counted.error().to_string();
        EXPECT_EQ(static_cast<size_t>(counted.value()), live_matching.size()) << label;

        auto top = searcher.search(query, num_asc.value(), 10);
        ASSERT_TRUE(top.has_value()) << top.error().to_string();
        EXPECT_EQ(static_cast<size_t>(top.value().total_hits.value), live_matching.size()) << label;
        EXPECT_EQ(top.value().total_hits.relation, TotalHits::Relation::EqualTo) << label;
        std::vector<int64_t> first_page;
        for (const FieldDoc &hit : top.value().score_docs) {
            first_page.push_back(std::get<int64_t>(hit.fields.front()));
        }
        const auto end = static_cast<size_t>(std::min<size_t>(10, matching_numbers.size()));
        EXPECT_EQ(first_page, std::vector<int64_t>(matching_numbers.begin(), matching_numbers.begin() + end)) << label;
    };

    Fixture deleted_fixture("clucene_sorted_differential_deleted", seed);
    build(deleted_fixture, std::nullopt, deleted, /*merge_to=*/0, /*no_merge=*/true);
    {
        auto directory = FSDirectory::open(deleted_fixture.path);
        ASSERT_TRUE(directory.has_value());
        auto reader = DirectoryReader::open(*directory.value());
        ASSERT_TRUE(reader.has_value());
        ASSERT_TRUE(reader.value()->has_deletions().value()) << "the deletions have to stay live";
    }
    check(deleted_fixture, 0, "deleted");

    Fixture merged_fixture("clucene_sorted_differential_deleted_merged", seed);
    build(merged_fixture, std::nullopt, deleted, /*merge_to=*/1);
    {
        auto directory = FSDirectory::open(merged_fixture.path);
        ASSERT_TRUE(directory.has_value());
        auto reader = DirectoryReader::open(*directory.value());
        ASSERT_TRUE(reader.has_value());
        EXPECT_EQ(reader.value()->leaves().value().size(), 1U);
        EXPECT_FALSE(reader.value()->has_deletions().value()) << "a merge drops the deleted documents";
    }
    check(merged_fixture, 1, "deleted and merged");
}
