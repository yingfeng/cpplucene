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
#include <memory>
#include <string>
#include <utility>

module lucene_core:ut.soft_deletes;

import lucene_core;

using namespace lucene;

namespace {

FieldType id_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::Docs).has_value());
    return type;
}

/// A document with an `id`, a `title` and the soft deletes field `sd`.
Document version(std::string id, std::string title, int64_t soft_delete_value) {
    Document doc;
    doc.add(std::make_unique<Field>("id", FieldValue{std::move(id)}, id_type()));
    auto sd = NumericDocValuesField::create("sd", soft_delete_value);
    EXPECT_TRUE(sd.has_value()) << (sd ? "" : sd.error().to_string());
    doc.add(std::make_unique<Field>(std::move(sd.value())));
    auto stored = FieldType();
    EXPECT_TRUE(stored.set_stored(true).has_value());
    doc.add(std::make_unique<Field>("title", FieldValue{std::move(title)}, stored));
    return doc;
}

Term id_term(std::string_view id) {
    return Term::from_text("id", id);
}

/// The titles the reader currently shows, in document order.
std::vector<std::string> titles(const std::shared_ptr<DirectoryReader> &reader) {
    std::vector<std::string> out;
    IndexSearcher searcher(reader);
    auto hits = searcher.search(MatchAllDocsQuery(), 10);
    EXPECT_TRUE(hits.has_value()) << (hits ? "" : hits.error().to_string());
    if (!hits.has_value()) {
        return out;
    }
    auto stored = reader->stored_fields();
    EXPECT_TRUE(stored.has_value());
    for (const ScoreDoc &hit : hits.value().score_docs) {
        auto document = stored.value()->document(hit.doc);
        EXPECT_TRUE(document.has_value());
        if (document.has_value()) {
            auto values = document.value().get_values("title");
            EXPECT_TRUE(values.has_value());
            if (values.has_value()) {
                out.push_back(values.value().empty() ? std::string{} : values.value().front());
            }
        }
    }
    return out;
}

} // namespace

/// A writer that never merges, so the commit counters stay observable.
Result<std::unique_ptr<IndexWriter>> open_without_merging(Directory &directory, IndexWriterConfig config) {
    config.set_merge_policy(NoMergePolicy::create());
    return IndexWriter::open(directory, config);
}

TEST(SoftDeletesTest, RequiresAConfiguredField) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(directory, IndexWriterConfig(analyzer));
    ASSERT_TRUE(writer.has_value());

    auto updated = writer.value()->soft_update_document(id_term("a"), version("a", "v1", 1));
    ASSERT_FALSE(updated.has_value());
    EXPECT_NE(updated.error().to_string().find("set_soft_deletes_field"), std::string::npos);
}

TEST(SoftDeletesTest, RequiresTheDocumentToCarryTheField) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_soft_deletes_field("sd");
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value());

    Document without;
    without.add(std::make_unique<Field>("id", FieldValue{std::string("a")}, id_type()));
    auto updated = writer.value()->soft_update_document(id_term("a"), without);
    ASSERT_FALSE(updated.has_value());
    EXPECT_NE(updated.error().to_string().find("has to carry"), std::string::npos);
}

TEST(SoftDeletesTest, ReplacesThePreviousVersionAndReportsItAsSoft) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_soft_deletes_field("sd");
    auto writer = open_without_merging(directory, config);
    ASSERT_TRUE(writer.has_value());

    ASSERT_TRUE(writer.value()->add_document(version("a", "v1", 1)).has_value());
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }

    ASSERT_TRUE(writer.value()->soft_update_document(id_term("a"), version("a", "v2", 2)).has_value());
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->num_docs().value(), 1);
    const auto shown = titles(reader.value());
    ASSERT_EQ(shown.size(), 1U);
    EXPECT_EQ(shown.front(), "v2");

    // The previous version is gone from its segment, and the commit says it went
    // away as a soft deletion.
    const SegmentCommitInfo &segment = writer.value()->segment_infos().segments[0];
    EXPECT_EQ(segment.soft_del_count, 1);
    EXPECT_EQ(segment.del_count, 0);
    EXPECT_EQ(segment.info.max_doc, 1);
    EXPECT_EQ(segment.doc_count(), 0);
    EXPECT_EQ(writer.value()->segment_infos().segments.size(), 2U);
}

TEST(SoftDeletesTest, HardDeletesStayInTheirOwnCounter) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_soft_deletes_field("sd");
    auto writer = open_without_merging(directory, config);
    ASSERT_TRUE(writer.has_value());

    ASSERT_TRUE(writer.value()->add_document(version("a", "v1", 1)).has_value());
    ASSERT_TRUE(writer.value()->add_document(version("b", "b1", 1)).has_value());
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }

    ASSERT_TRUE(writer.value()->soft_update_document(id_term("a"), version("a", "v2", 2)).has_value());
    ASSERT_TRUE(writer.value()->delete_documents(id_term("b")).has_value());
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    EXPECT_EQ(reader.value()->num_docs().value(), 1);
    EXPECT_EQ(titles(reader.value()).front(), "v2");

    const SegmentCommitInfo &segment = writer.value()->segment_infos().segments[0];
    EXPECT_EQ(segment.info.max_doc, 2);
    EXPECT_EQ(segment.soft_del_count, 1);
    EXPECT_EQ(segment.del_count, 1);
    EXPECT_EQ(segment.doc_count(), 0);
    EXPECT_EQ(segment.total_del_count(), 2);
}

TEST(IndexWriterApisTest, AddsDocumentsInOneCall) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(directory, IndexWriterConfig(analyzer));
    ASSERT_TRUE(writer.has_value());

    Document first = version("a", "one", 1);
    Document second = version("b", "two", 1);
    const Document *documents[] = {&first, &second, nullptr};
    auto added = writer.value()->add_documents(std::span<const Document *const>(documents, 2));
    ASSERT_TRUE(added.has_value()) << (added ? "" : added.error().to_string());
    EXPECT_EQ(writer.value()->num_buffered_documents(), 2);

    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }
    EXPECT_EQ(writer.value()->segment_count(), 1U);

    // A null document is rejected instead of being skipped.
    auto rejected = writer.value()->add_documents(std::span<const Document *const>(documents + 2, 1));
    EXPECT_FALSE(rejected.has_value());
}

TEST(IndexWriterApisTest, ReportsUncommittedChanges) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(directory, IndexWriterConfig(analyzer));
    ASSERT_TRUE(writer.has_value());
    EXPECT_FALSE(writer.value()->has_uncommitted_changes());

    ASSERT_TRUE(writer.value()->add_document(version("a", "one", 1)).has_value());
    EXPECT_TRUE(writer.value()->has_uncommitted_changes());

    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }
    EXPECT_FALSE(writer.value()->has_uncommitted_changes());

    // A buffered delete counts too, before it reaches the `.liv`.
    ASSERT_TRUE(writer.value()->delete_documents(id_term("a")).has_value());
    EXPECT_TRUE(writer.value()->has_uncommitted_changes());
}

TEST(IndexWriterApisTest, RecordsLiveCommitData) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(directory, IndexWriterConfig(analyzer));
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer.value()->add_document(version("a", "one", 1)).has_value());

    writer.value()->set_live_commit_data({{"author", "clucene"}, {"build", "test"}});
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }
    EXPECT_EQ(writer.value()->segment_infos().user_data.at("author"), "clucene");

    auto commits = DirectoryReader::list_commits(directory);
    ASSERT_TRUE(commits.has_value()) << (commits ? "" : commits.error().to_string());
    ASSERT_FALSE(commits.value().empty());
    EXPECT_EQ(commits.value().front()->user_data().at("build"), "test");
}
