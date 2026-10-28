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

// A randomized run of adds, updates, deletes, commits and merges, checked against
// a model of the documents: after every phase the index has to hold exactly the
// live documents, with the right content, and `CheckIndex` has to be clean.

module;

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.e2e_update_differential;

import lucene_core;

using namespace lucene;

namespace {

constexpr int32_t kIds = 24;
constexpr std::string_view kIdField = "id";
constexpr std::string_view kBodyField = "body";

FieldType id_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::Docs).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

FieldType body_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

Document make_document(int32_t id, const std::string &body) {
    Document document;
    document.add(std::make_unique<Field>(std::string(kIdField), FieldValue{std::to_string(id)}, id_type()));
    document.add(std::make_unique<Field>(std::string(kBodyField), FieldValue{body}, body_type()));
    return document;
}

Term id_term(int32_t id) { return Term::from_text(std::string(kIdField), std::to_string(id)); }

/// The documents a query matches.
std::vector<int32_t> hits_of(IndexSearcher &searcher, const Query &query, int32_t limit = kIds) {
    auto hits = searcher.search(query, limit);
    EXPECT_TRUE(hits.has_value()) << (hits.has_value() ? "" : hits.error().to_string());
    std::vector<int32_t> docs;
    if (!hits.has_value()) {
        return docs;
    }
    for (const ScoreDoc &hit : hits.value().score_docs) {
        docs.push_back(hit.doc);
    }
    std::sort(docs.begin(), docs.end());
    return docs;
}

/// What the index has to hold for one id: the bodies of the documents that carry
/// it.  An `add_document` may repeat an id, an `update_document` leaves exactly one.
using Model = std::map<int32_t, std::vector<std::string>>;

/// Asserts the index holds exactly `model`, including repeated ids.
void expect_matches_model(const std::shared_ptr<DirectoryReader> &reader, const Model &model,
                          std::string_view phase) {
    IndexSearcher searcher(reader);
    size_t expected_docs = 0;
    for (const auto &[id, bodies] : model) {
        (void)id;
        expected_docs += bodies.size();
    }

    auto count_all = searcher.count(MatchAllDocsQuery());
    ASSERT_TRUE(count_all.has_value()) << phase << ": " << count_all.error().to_string();
    EXPECT_EQ(static_cast<size_t>(count_all.value()), expected_docs) << phase;

    auto exists = searcher.count(FieldExistsQuery(std::string(kBodyField)));
    ASSERT_TRUE(exists.has_value()) << phase << ": " << exists.error().to_string();
    EXPECT_EQ(static_cast<size_t>(exists.value()), expected_docs) << phase;

    for (int32_t id = 0; id < kIds; ++id) {
        const TermQuery query(id_term(id));
        const std::vector<int32_t> hits = hits_of(searcher, query);
        const auto found = model.find(id);
        if (found == model.end()) {
            EXPECT_TRUE(hits.empty()) << phase << ": id " << id << " was deleted but is still indexed";
            continue;
        }
        ASSERT_EQ(hits.size(), found->second.size())
                << phase << ": id " << id << " is indexed " << hits.size() << " times";
        // The bodies of the hits have to be exactly the ones the model holds; one
        // stored fields cursor answers for every hit of this id.
        auto stored = reader->stored_fields();
        ASSERT_TRUE(stored.has_value());
        std::vector<std::string> bodies;
        for (const int32_t hit : hits) {
            auto document = stored.value()->document(hit);
            ASSERT_TRUE(document.has_value());
            auto ids = document.value().get_values(std::string(kIdField));
            ASSERT_TRUE(ids.has_value());
            ASSERT_EQ(ids.value().size(), 1U);
            EXPECT_EQ(ids.value().front(), std::to_string(id)) << phase << ": wrong id field";
            auto values = document.value().get_values(std::string(kBodyField));
            ASSERT_TRUE(values.has_value());
            ASSERT_EQ(values.value().size(), 1U);
            bodies.push_back(values.value().front());
        }
        std::vector<std::string> expected = found->second;
        std::sort(bodies.begin(), bodies.end());
        std::sort(expected.begin(), expected.end());
        EXPECT_EQ(bodies, expected) << phase << ": id " << id << " holds other bodies";
    }
}

void check_index(const std::filesystem::path &path) {
    auto directory = FSDirectory::open(path);
    ASSERT_TRUE(directory.has_value());
    CheckIndex check(*directory.value());
    auto checked = check.check();
    ASSERT_TRUE(checked.has_value()) << (checked.has_value() ? "" : checked.error().to_string());
    EXPECT_TRUE(checked.value().clean()) << checked.value().to_string();
}

} // namespace

TEST(UpdateDifferentialTest, ARandomRunOfUpdatesAndDeletesAgreesWithTheModel) {
    uint32_t seed = 20261025;
    if (const char *configured = std::getenv("CLUCENE_DIFF_SEED"); configured != nullptr) {
        seed = static_cast<uint32_t>(std::strtoul(configured, nullptr, 10));
    }
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "clucene_update_differential";
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);

    auto directory = FSDirectory::open(path);
    ASSERT_TRUE(directory.has_value());
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_max_buffered_docs(17); // a few segments, and merges along the way
    auto writer = IndexWriter::open(*directory.value(), config);
    ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());

    std::mt19937 engine(seed);
    std::uniform_int_distribution<int32_t> id_pick(0, kIds - 1);
    std::uniform_int_distribution<int32_t> op_pick(0, 9);
    Model model;
    int32_t body_counter = 0;

    for (int32_t step = 0; step < 80; ++step) {
        const int32_t id = id_pick(engine);
        const std::string body = "body" + std::to_string(body_counter++);
        switch (op_pick(engine)) {
            case 0:
            case 1: // a plain add, which may repeat an existing id
                ASSERT_TRUE(writer.value()->add_document(make_document(id, body)).has_value());
                model[id].push_back(body);
                break;
            case 2:
            case 3: // an update: every document of that id is replaced by one
                ASSERT_TRUE(writer.value()->update_document(id_term(id), make_document(id, body)).has_value());
                model[id] = {body};
                break;
            case 4:
            case 5: // delete, which removes every document of that id
                ASSERT_TRUE(writer.value()->delete_documents(id_term(id)).has_value());
                model.erase(id);
                break;
            case 6: // a commit, which is also what merges happen on
                ASSERT_TRUE(writer.value()->commit().has_value());
                break;
            case 7: // an explicit merge, rarely, because it rewrites the whole index
                if (step % 27 == 0) {
                    ASSERT_TRUE(writer.value()->force_merge(1).has_value());
                } else {
                    ASSERT_TRUE(writer.value()->commit().has_value());
                }
                break;
            case 8: // a delete by query
                ASSERT_TRUE(writer.value()
                                    ->delete_documents(std::make_shared<TermQuery>(id_term(id)))
                                    .has_value());
                model.erase(id);
                break;
            default: // a delete that hits nothing is a no-op for the model
                ASSERT_TRUE(writer.value()->delete_documents(Term::from_text(std::string(kIdField), "none")).has_value());
                break;
        }

        if (step % 27 == 0) {
            ASSERT_TRUE(writer.value()->commit().has_value());
            auto reader = DirectoryReader::open(*directory.value());
            ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
            expect_matches_model(reader.value(), model, "step " + std::to_string(step));
            // A NRT reader sees the same documents.
            auto nrt = writer.value()->get_reader();
            ASSERT_TRUE(nrt.has_value()) << (nrt.has_value() ? "" : nrt.error().to_string());
            expect_matches_model(nrt.value(), model, "step " + std::to_string(step) + " (NRT)");
        }
    }

    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->force_merge(1).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());

    auto reader = DirectoryReader::open(*directory.value());
    ASSERT_TRUE(reader.has_value());
    ASSERT_EQ(reader.value()->leaves().value().size(), 1U);
    expect_matches_model(reader.value(), model, "after the merge");
    check_index(path);

    std::filesystem::remove_all(path, ignored);
}
