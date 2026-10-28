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
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <vector>

module lucene_core:ut.e2e_differential;

import lucene_core;

using namespace lucene;

namespace {

constexpr int32_t DOC_COUNT = 200;
constexpr int32_t VOCAB_SIZE = 12;

/// The vocabulary: `t0` .. `t11`, plus a shared `common` term that every
/// document carries so that Boolean queries always have something to intersect.
const std::vector<std::string> &vocabulary() {
    static const std::vector<std::string> words = [] {
        std::vector<std::string> out;
        for (int32_t i = 0; i < VOCAB_SIZE; ++i) {
            out.push_back("t" + std::to_string(i));
        }
        return out;
    }();
    return words;
}

/// One source document, mirroring what the index stores.
struct SourceDoc {
    std::vector<std::string> terms;
    int64_t number{0};
    int32_t point{0};
    std::string tag;
};

/// The index that was built from a list of source documents.
struct Fixture {
    std::filesystem::path path;
    std::vector<SourceDoc> docs;

    Fixture(std::string_view name, uint32_t seed) {
        path = std::filesystem::temp_directory_path() / std::string(name);
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);

        std::mt19937 engine(seed);
        std::uniform_int_distribution<int32_t> term_pick(0, VOCAB_SIZE - 1);
        std::uniform_int_distribution<int32_t> term_count(1, 4);
        std::uniform_int_distribution<int64_t> number_pick(0, 99);
        std::uniform_int_distribution<int32_t> point_pick(0, 49);
        std::uniform_int_distribution<int32_t> tag_pick(0, 3);

        for (int32_t id = 0; id < DOC_COUNT; ++id) {
            SourceDoc doc;
            doc.terms.push_back("common");
            const int32_t count = term_count(engine);
            for (int32_t i = 0; i < count; ++i) {
                doc.terms.push_back(vocabulary()[static_cast<size_t>(term_pick(engine))]);
            }
            std::sort(doc.terms.begin(), doc.terms.end());
            doc.terms.erase(std::unique(doc.terms.begin(), doc.terms.end()), doc.terms.end());
            doc.number = number_pick(engine);
            doc.point = point_pick(engine);
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
    // Positions are needed by the phrase queries the differential test runs.
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqsAndPositions).has_value());
    return type;
}

FieldType id_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::Docs).has_value());
    return type;
}

/// The document id, stored so that the differential test can compare documents
/// across merges that renumber them.
FieldType stored_id_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::Docs).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

Document make_document(const SourceDoc &source, int32_t id) {
    Document doc;
    doc.add(std::make_unique<Field>("id", FieldValue{std::to_string(id)}, stored_id_type()));
    for (const std::string &term : source.terms) {
        doc.add(std::make_unique<Field>("body", FieldValue{term}, text_type()));
    }
    auto number = NumericDocValuesField::create("num", source.number);
    EXPECT_TRUE(number.has_value());
    doc.add(std::make_unique<Field>(std::move(number.value())));
    doc.add(std::make_unique<Field>("tag", FieldValue{source.tag}, id_type()));
    auto tag_values = SortedSetDocValuesField::create("tagdv", source.tag);
    EXPECT_TRUE(tag_values.has_value());
    doc.add(std::make_unique<Field>(std::move(tag_values.value())));
    auto sorted_number = SortedNumericDocValuesField::create("snum", source.number);
    EXPECT_TRUE(sorted_number.has_value());
    doc.add(std::make_unique<Field>(std::move(sorted_number.value())));

    const int32_t point[1] = {source.point};
    auto points = IntPoint::create("pt", std::span<const int32_t>(point, 1));
    EXPECT_TRUE(points.has_value());
    doc.add(std::make_unique<Field>(std::move(points.value())));
    return doc;
}

/// Writes every document of `fixture` into a fresh index, index-sorted when
/// `index_sort` is given.
void build(Fixture &fixture, const std::optional<Sort> &index_sort = std::nullopt) {
    auto directory = FSDirectory::open(fixture.path);
    ASSERT_TRUE(directory.has_value());
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_max_buffered_docs(37);
    if (index_sort.has_value()) {
        config.set_index_sort(index_sort.value());
    }
    auto writer = IndexWriter::open(*directory.value(), config);
    ASSERT_TRUE(writer.has_value());
    for (int32_t id = 0; id < DOC_COUNT; ++id) {
        ASSERT_TRUE(writer.value()->add_document(make_document(fixture.docs[static_cast<size_t>(id)], id)).has_value());
    }
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());
}

/// The documents `query` matches, according to the source data.
std::vector<int32_t> oracle(const std::vector<SourceDoc> &docs, const std::vector<int32_t> &live,
                            const std::function<bool(const SourceDoc &)> &predicate) {
    std::vector<int32_t> hits;
    for (int32_t id : live) {
        if (predicate(docs[static_cast<size_t>(id)])) {
            hits.push_back(id);
        }
    }
    return hits;
}

bool has_term(const SourceDoc &doc, std::string_view term) {
    return std::binary_search(doc.terms.begin(), doc.terms.end(), term);
}

/// The stored `id` values of the hits, sorted.
///
/// Document ids are not stable across a merge that drops documents, so the
/// deleted phase compares the stored id instead of the internal one.
std::vector<int32_t> search_stored_ids(IndexSearcher &searcher, DirectoryReader &reader, const Query &query,
                                       int32_t limit) {
    auto hits = searcher.search(query, limit);
    EXPECT_TRUE(hits.has_value()) << (hits ? "" : hits.error().to_string());
    std::vector<int32_t> ids;
    if (!hits.has_value()) {
        return ids;
    }
    auto stored = reader.stored_fields();
    EXPECT_TRUE(stored.has_value());
    if (!stored.has_value()) {
        return ids;
    }
    for (const ScoreDoc &hit : hits.value().score_docs) {
        auto document = stored.value()->document(hit.doc);
        EXPECT_TRUE(document.has_value());
        if (!document.has_value()) {
            continue;
        }
        auto values = document.value().get_values("id");
        EXPECT_TRUE(values.has_value());
        if (values.has_value() && !values.value().empty()) {
            ids.push_back(std::atoi(values.value().front().c_str()));
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

/// Runs `CheckIndex` over the index and asserts it is clean.
void check_index(const std::filesystem::path &path, int32_t expected_docs) {
    auto directory = FSDirectory::open(path);
    ASSERT_TRUE(directory.has_value());
    CheckIndex check(*directory.value());
    auto checked = check.check();
    ASSERT_TRUE(checked.has_value()) << (checked ? "" : checked.error().to_string());
    EXPECT_TRUE(checked.value().clean()) << checked.value().to_string();
    EXPECT_EQ(checked.value().total_docs(), expected_docs);
}

/// Asserts that `count(query)` agrees with a full search, and that both agree
/// with the brute-force answer.
///
/// `count()` rewrites the query through `ConstantScoreQuery` when scores are not
/// needed, so this cross-check covers a different execution path than
/// `search()`.  A divergence here is what the `minimum_should_match` over-count
/// looked like.
void expect_count_agrees(IndexSearcher &searcher, const Query &query, std::vector<int32_t> expected,
                         std::string_view label) {
    std::sort(expected.begin(), expected.end());
    auto counted = searcher.count(query);
    ASSERT_TRUE(counted.has_value()) << label << ": " << (counted ? "" : counted.error().to_string());
    EXPECT_EQ(static_cast<size_t>(counted.value()), expected.size()) << "count: " << label;

    auto hits = searcher.search(query, DOC_COUNT);
    ASSERT_TRUE(hits.has_value()) << label << ": " << (hits ? "" : hits.error().to_string());
    EXPECT_EQ(hits.value().total_hits.relation, TotalHits::Relation::EqualTo) << label;
    EXPECT_EQ(hits.value().total_hits.value, expected.size()) << "total_hits: " << label;
    EXPECT_EQ(hits.value().score_docs.size(), expected.size()) << "hits: " << label;
}

void expect_same(const std::vector<int32_t> &actual, std::vector<int32_t> expected, std::string_view label) {
    std::sort(expected.begin(), expected.end());
    ASSERT_EQ(actual.size(), expected.size()) << label;
    for (size_t i = 0; i < actual.size(); ++i) {
        ASSERT_EQ(actual[i], expected[i]) << label << " at position " << i;
    }
}

} // namespace

/// Runs a fixed set of queries against an index and against the source data.
///
/// The point is not to test one query type but the *combination* of codec
/// writing, posting/point/doc-values reading, query rewriting and top-doc
/// collection: every query has to agree with a brute force scan of the source
/// documents, before and after a forced merge.
TEST(DifferentialTest, QueriesAgreeWithABruteForceScan) {
    // The seed is fixed so that a failure is reproducible; set
    // `CLUCENE_DIFF_SEED` to explore other document sets.
    uint32_t seed = 20261025;
    if (const char *configured = std::getenv("CLUCENE_DIFF_SEED"); configured != nullptr) {
        seed = static_cast<uint32_t>(std::strtoul(configured, nullptr, 10));
    }
    Fixture fixture("lucene_core_differential_ut", seed);
    build(fixture);

    std::vector<int32_t> live(DOC_COUNT);
    std::iota(live.begin(), live.end(), 0);

    auto directory = FSDirectory::open(fixture.path);
    ASSERT_TRUE(directory.has_value());

    // 1. As written, spread over several segments.
    {
        auto reader = DirectoryReader::open(*directory.value());
        ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
        ASSERT_EQ(reader.value()->num_docs().value(), DOC_COUNT);
        IndexSearcher searcher(reader.value());

        // Every vocabulary term.
        for (const std::string &term : vocabulary()) {
            TermQuery query(Term::from_text("body", term));
            expect_same(search_stored_ids(searcher, *reader.value(), query, DOC_COUNT),
                        oracle(fixture.docs, live, [&](const SourceDoc &doc) { return has_term(doc, term); }),
                        "term " + term);
        }

        // A boolean query: `t0` MUST, `t1` SHOULD, `t2` MUST NOT.
        {
            BooleanQuery::Builder builder;
            ASSERT_TRUE(builder.add(std::make_shared<TermQuery>(Term::from_text("body", "t0")), Occur::Must).has_value());
            ASSERT_TRUE(builder.add(std::make_shared<TermQuery>(Term::from_text("body", "t1")), Occur::Should).has_value());
            ASSERT_TRUE(
                    builder.add(std::make_shared<TermQuery>(Term::from_text("body", "t2")), Occur::MustNot).has_value());
            auto query = builder.build();
            ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());
            expect_same(search_stored_ids(searcher, *reader.value(), query.value(), DOC_COUNT),
                        oracle(fixture.docs, live,
                               [](const SourceDoc &doc) {
                                   return has_term(doc, "t0") && !has_term(doc, "t2");
                               }),
                        "boolean");
        }

        // A pure disjunction.
        {
            BooleanQuery::Builder builder;
            ASSERT_TRUE(builder.add(std::make_shared<TermQuery>(Term::from_text("body", "t3")), Occur::Should).has_value());
            ASSERT_TRUE(builder.add(std::make_shared<TermQuery>(Term::from_text("body", "t4")), Occur::Should).has_value());
            auto query = builder.build();
            ASSERT_TRUE(query.has_value());
            expect_same(search_stored_ids(searcher, *reader.value(), query.value(), DOC_COUNT),
                        oracle(fixture.docs, live,
                               [](const SourceDoc &doc) { return has_term(doc, "t3") || has_term(doc, "t4"); }),
                        "disjunction");
        }

        // A prefix query over the whole vocabulary: `t1` also matches `t10`, `t11`.
        {
            PrefixQuery query("body", "t1");
            expect_same(search_stored_ids(searcher, *reader.value(), query, DOC_COUNT),
                        oracle(fixture.docs, live,
                               [](const SourceDoc &doc) {
                                   return std::any_of(doc.terms.begin(), doc.terms.end(),
                                                      [](const std::string &term) { return term.starts_with("t1"); });
                               }),
                        "prefix t1");
        }

        // A point range.
        {
            auto query = IntPoint::new_range_query("pt", 10, 20);
            ASSERT_TRUE(query.has_value());
            expect_same(search_stored_ids(searcher, *reader.value(), *query.value(), DOC_COUNT),
                        oracle(fixture.docs, live, [](const SourceDoc &doc) { return doc.point >= 10 && doc.point <= 20; }),
                        "point range");
        }

        // A phrase: `common` immediately followed by `t5`, which only holds for
        // documents whose sorted term list is exactly `{common, t5}`.
        {
            auto terms = std::vector<Term>{Term::from_text("body", "common"), Term::from_text("body", "t5")};
            auto query = PhraseQuery::create("body", terms);
            ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());
            expect_same(search_stored_ids(searcher, *reader.value(), query.value(), DOC_COUNT),
                        oracle(fixture.docs, live,
                               [](const SourceDoc &doc) {
                                   // The terms are indexed in sorted order, so the
                                   // phrase holds exactly when `t5` comes right
                                   // after `common`.
                                   return doc.terms.size() >= 2 && doc.terms[0] == "common" && doc.terms[1] == "t5";
                               }),
                        "phrase common t5");
        }

        // A wildcard, which takes the automaton rewriting path.
        {
            auto query = WildcardQuery::create(Term::from_text("body", "t1*"));
            ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());
            expect_same(search_stored_ids(searcher, *reader.value(), query.value(), DOC_COUNT),
                        oracle(fixture.docs, live,
                               [](const SourceDoc &doc) {
                                   return std::any_of(doc.terms.begin(), doc.terms.end(),
                                                      [](const std::string &term) { return term.starts_with("t1"); });
                               }),
                        "wildcard t1*");
        }

        // A sorted numeric doc values range.
        {
            auto query = SortedNumericDocValuesField::new_slow_range_query("snum", 20, 39);
            ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());
            expect_same(search_stored_ids(searcher, *reader.value(), query.value(), DOC_COUNT),
                        oracle(fixture.docs, live, [](const SourceDoc &doc) { return doc.number >= 20 && doc.number <= 39; }),
                        "sorted numeric range");
        }

        // A sorted set doc values range.
        {
            auto lower = BytesRef::from_string("tag1");
            auto upper = BytesRef::from_string("tag2");
            auto query = SortedSetDocValuesField::new_slow_range_query("tagdv", lower, upper, true, true);
            ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());
            expect_same(search_stored_ids(searcher, *reader.value(), query.value(), DOC_COUNT),
                        oracle(fixture.docs, live,
                               [](const SourceDoc &doc) { return doc.tag >= "tag1" && doc.tag <= "tag2"; }),
                        "sorted set range");
        }

        // A single term filter via MatchAllDocs, to check the collector count.
        {
            auto all = searcher.count(MatchAllDocsQuery());
            ASSERT_TRUE(all.has_value());
            EXPECT_EQ(all.value(), DOC_COUNT);
        }
    }
    check_index(fixture.path, DOC_COUNT);

    // 2. After a forced merge the same queries must return the same documents.
    {
        WhitespaceAnalyzer analyzer;
        IndexWriterConfig config(analyzer);
        auto writer = IndexWriter::open(*directory.value(), config);
        ASSERT_TRUE(writer.has_value());
        ASSERT_TRUE(writer.value()->force_merge(1).has_value());
        ASSERT_TRUE(writer.value()->commit().has_value());
        ASSERT_TRUE(writer.value()->close().has_value());
    }

    auto merged = DirectoryReader::open(*directory.value());
    ASSERT_TRUE(merged.has_value()) << (merged ? "" : merged.error().to_string());
    EXPECT_EQ(merged.value()->leaves().value().size(), 1U);
    EXPECT_EQ(merged.value()->num_docs().value(), DOC_COUNT);

    IndexSearcher searcher(merged.value());

    for (const std::string &term : vocabulary()) {
        TermQuery query(Term::from_text("body", term));
        expect_same(search_stored_ids(searcher, *merged.value(), query, DOC_COUNT),
                    oracle(fixture.docs, live, [&](const SourceDoc &doc) { return has_term(doc, term); }),
                    "merged term " + term);
    }
    {
        auto query = IntPoint::new_range_query("pt", 30, 39);
        ASSERT_TRUE(query.has_value());
        expect_same(search_stored_ids(searcher, *merged.value(), *query.value(), DOC_COUNT),
                    oracle(fixture.docs, live, [](const SourceDoc &doc) { return doc.point >= 30 && doc.point <= 39; }),
                    "merged point range");
    }

    check_index(fixture.path, DOC_COUNT);

    // 3. Delete every even document and check the queries still agree.
    {
        WhitespaceAnalyzer merge_analyzer;
        auto writable = IndexWriter::open(*directory.value(), IndexWriterConfig(merge_analyzer));
        ASSERT_TRUE(writable.has_value());
        for (int32_t id = 0; id < DOC_COUNT; id += 2) {
            auto tagged = Term::from_text("id", std::to_string(id));
            ASSERT_TRUE(writable.value()->delete_documents(tagged).has_value());
        }
        ASSERT_TRUE(writable.value()->force_merge(1).has_value());
        ASSERT_TRUE(writable.value()->commit().has_value());
        ASSERT_TRUE(writable.value()->close().has_value());
    }

    std::vector<int32_t> surviving;
    for (int32_t id = 1; id < DOC_COUNT; id += 2) {
        surviving.push_back(id);
    }

    auto after_deletes = DirectoryReader::open(*directory.value());
    ASSERT_TRUE(after_deletes.has_value()) << (after_deletes ? "" : after_deletes.error().to_string());
    EXPECT_EQ(after_deletes.value()->num_docs().value(), static_cast<int32_t>(surviving.size()));
    IndexSearcher after_searcher(after_deletes.value());

    for (const std::string &term : vocabulary()) {
        TermQuery query(Term::from_text("body", term));
        expect_same(search_stored_ids(after_searcher, *after_deletes.value(), query, DOC_COUNT),
                    oracle(fixture.docs, surviving, [&](const SourceDoc &doc) { return has_term(doc, term); }),
                    "deleted term " + term);
    }
    {
        auto query = IntPoint::new_range_query("pt", 0, 49);
        ASSERT_TRUE(query.has_value());
        expect_same(search_stored_ids(after_searcher, *after_deletes.value(), *query.value(), DOC_COUNT),
                    oracle(fixture.docs, surviving, [](const SourceDoc &doc) { return true; }),
                    "deleted point range");
    }
    // The seek paths (`seek_exact` / `seek_ceil` on terms that may not exist) now
    // run against a merged segment with deletions, which is the state that used
    // to trip an internal frame assertion.
    for (const std::string &prefix : {std::string("t"), std::string("t0"), std::string("t1"), std::string("t9"),
                                      std::string("zz"), std::string("comm")}) {
        PrefixQuery query("body", prefix);
        expect_same(search_stored_ids(after_searcher, *after_deletes.value(), query, DOC_COUNT),
                    oracle(fixture.docs, surviving,
                           [&prefix](const SourceDoc &doc) {
                               return std::any_of(doc.terms.begin(), doc.terms.end(),
                                                  [&prefix](const std::string &term) { return term.starts_with(prefix); });
                           }),
                    "deleted prefix " + prefix);
    }
    {
        // A term that is in no document at all: the seek has to fail cleanly.
        TermQuery missing(Term::from_text("body", "nope"));
        expect_same(search_stored_ids(after_searcher, *after_deletes.value(), missing, DOC_COUNT),
                    std::vector<int32_t>{}, "missing term");
        // And a term that exists, found through the point index instead.
        TermQuery present(Term::from_text("body", "common"));
        expect_same(search_stored_ids(after_searcher, *after_deletes.value(), present, DOC_COUNT),
                    oracle(fixture.docs, surviving, [](const SourceDoc &doc) { return has_term(doc, "common"); }),
                    "common term after deletes");
    }

    check_index(fixture.path, static_cast<int32_t>(surviving.size()));

    // 4. A doc values update has to be visible to a reader opened afterwards.
    const int64_t updated_value = 1000;
    const int32_t updated_source_id = 1;
    {
        WhitespaceAnalyzer analyzer;
        auto writable = IndexWriter::open(*directory.value(), IndexWriterConfig(analyzer));
        ASSERT_TRUE(writable.has_value());
        auto updated = writable.value()->update_numeric_doc_value(Term::from_text("id", std::to_string(updated_source_id)),
                                                                 "num", updated_value);
        ASSERT_TRUE(updated.has_value()) << (updated ? "" : updated.error().to_string());
        ASSERT_TRUE(writable.value()->commit().has_value());
        ASSERT_TRUE(writable.value()->close().has_value());
    }

    auto after_update = DirectoryReader::open(*directory.value());
    ASSERT_TRUE(after_update.has_value()) << (after_update ? "" : after_update.error().to_string());
    IndexSearcher update_searcher(after_update.value());
    {
        // Find the document through its stored id, then read its doc values.
        auto hits = update_searcher.search(TermQuery(Term::from_text("id", std::to_string(updated_source_id))), 10);
        ASSERT_TRUE(hits.has_value()) << (hits ? "" : hits.error().to_string());
        ASSERT_EQ(hits.value().score_docs.size(), 1U);
        const int32_t doc = hits.value().score_docs.front().doc;
        auto values = after_update.value()->leaves().value()[0].reader()->get_numeric_doc_values("num");
        ASSERT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
        ASSERT_NE(values.value(), nullptr);
        ASSERT_TRUE(values.value()->advance_exact(doc).value());
        EXPECT_EQ(values.value()->long_value().value(), updated_value);
    }
}

/// The boolean shapes whose `count()` used to disagree with `search()`: a
/// `minimum_should_match` disjunction, the same query wrapped in a constant
/// score, a filter combined with a minimum, and a nested boolean.  Every shape
/// is checked on the multi-segment index and again after a forced merge.
TEST(DifferentialTest, CountAgreesWithSearchOnBooleanShapes) {
    uint32_t seed = 20261025;
    if (const char *configured = std::getenv("CLUCENE_DIFF_SEED"); configured != nullptr) {
        seed = static_cast<uint32_t>(std::strtoul(configured, nullptr, 10));
    }
    Fixture fixture("lucene_core_differential_count_ut", seed);
    build(fixture);

    std::vector<int32_t> live(DOC_COUNT);
    std::iota(live.begin(), live.end(), 0);

    auto directory = FSDirectory::open(fixture.path);
    ASSERT_TRUE(directory.has_value());

    const auto terms_matched = [](const SourceDoc &doc, std::initializer_list<std::string_view> terms) {
        int32_t matched = 0;
        for (const std::string_view term : terms) {
            if (has_term(doc, term)) {
                ++matched;
            }
        }
        return matched;
    };

    // The queries and their brute-force answers, built once and reused on both
    // index shapes.
    std::vector<std::pair<std::string, std::shared_ptr<Query>>> queries;
    std::vector<std::vector<int32_t>> expected;

    for (const int32_t minimum : {2, 3}) {
        BooleanQuery::Builder builder;
        (void)builder.set_minimum_number_should_match(minimum);
        for (const std::string_view term : {"t0", "t1", "t2"}) {
            ASSERT_TRUE(builder.add(std::make_shared<TermQuery>(Term::from_text("body", term)),
                                    Occur::Should)
                                .has_value());
        }
        auto query = builder.build();
        ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());
        const std::string label = "minimum_should_match=" + std::to_string(minimum);
        queries.emplace_back(label, std::make_shared<BooleanQuery>(query.value()));
        expected.push_back(oracle(fixture.docs, live, [&](const SourceDoc &doc) {
            return terms_matched(doc, {"t0", "t1", "t2"}) >= minimum;
        }));

        // The constant score wrapper must keep the confirmation step.
        queries.emplace_back("constant score, " + label,
                             std::make_shared<ConstantScoreQuery>(std::make_shared<BooleanQuery>(query.value())));
        expected.push_back(expected.back());
    }

    {
        // A filter plus a minimum: `t0` and `t1` have to match, `common` filters.
        BooleanQuery::Builder builder;
        (void)builder.set_minimum_number_should_match(2);
        ASSERT_TRUE(builder.add(std::make_shared<TermQuery>(Term::from_text("body", "t0")), Occur::Should).has_value());
        ASSERT_TRUE(builder.add(std::make_shared<TermQuery>(Term::from_text("body", "t1")), Occur::Should).has_value());
        ASSERT_TRUE(builder.add(std::make_shared<TermQuery>(Term::from_text("body", "common")), Occur::Filter)
                            .has_value());
        auto query = builder.build();
        ASSERT_TRUE(query.has_value());
        queries.emplace_back("filter + minimum_should_match", std::make_shared<BooleanQuery>(query.value()));
        expected.push_back(oracle(fixture.docs, live,
                                  [&](const SourceDoc &doc) { return terms_matched(doc, {"t0", "t1"}) == 2; }));
    }

    {
        // A nested boolean: MUST (t4 OR t5), MUST NOT t6.
        BooleanQuery::Builder inner;
        for (const std::string_view term : {"t4", "t5"}) {
            ASSERT_TRUE(inner.add(std::make_shared<TermQuery>(Term::from_text("body", term)), Occur::Should)
                                .has_value());
        }
        auto inner_query = inner.build();
        ASSERT_TRUE(inner_query.has_value());
        BooleanQuery::Builder outer;
        ASSERT_TRUE(outer.add(std::make_shared<BooleanQuery>(inner_query.value()), Occur::Must).has_value());
        ASSERT_TRUE(outer.add(std::make_shared<TermQuery>(Term::from_text("body", "t6")), Occur::MustNot).has_value());
        auto query = outer.build();
        ASSERT_TRUE(query.has_value());
        queries.emplace_back("nested boolean", std::make_shared<BooleanQuery>(query.value()));
        expected.push_back(oracle(fixture.docs, live, [&](const SourceDoc &doc) {
            return (has_term(doc, "t4") || has_term(doc, "t5")) && !has_term(doc, "t6");
        }));
    }

    const auto run = [&](const std::shared_ptr<DirectoryReader> &reader) {
        IndexSearcher searcher(reader);
        for (size_t i = 0; i < queries.size(); ++i) {
            expect_count_agrees(searcher, *queries[i].second, expected[i], queries[i].first);
            expect_same(search_stored_ids(searcher, *reader, *queries[i].second, DOC_COUNT), expected[i],
                        "stored ids: " + queries[i].first);
        }
    };

    {
        auto reader = DirectoryReader::open(*directory.value());
        ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
        ASSERT_GT(reader.value()->leaves().value().size(), 1U);
        run(reader.value());
        ASSERT_TRUE(reader.value()->close().has_value());
    }
    check_index(fixture.path, DOC_COUNT);

    // The same queries on a single merged segment.
    {
        WhitespaceAnalyzer analyzer;
        IndexWriterConfig config(analyzer);
        auto writer = IndexWriter::open(*directory.value(), config);
        ASSERT_TRUE(writer.has_value());
        ASSERT_TRUE(writer.value()->force_merge(1).has_value());
        ASSERT_TRUE(writer.value()->commit().has_value());
        ASSERT_TRUE(writer.value()->close().has_value());
    }
    {
        auto reader = DirectoryReader::open(*directory.value());
        ASSERT_TRUE(reader.has_value());
        ASSERT_EQ(reader.value()->leaves().value().size(), 1U);
        run(reader.value());
        ASSERT_TRUE(reader.value()->close().has_value());
    }
    check_index(fixture.path, DOC_COUNT);
}
