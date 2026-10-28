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

// The retention query of a soft delete: a document that matches it survives the
// delete — it stays live and keeps its data — while every other document is only
// hidden, which is what a version chain or a time window needs.

module;

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.soft_deletes_retention;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kSoftDeletesField = "sd";
constexpr std::string_view kKeepTerm = "keep";

FieldType id_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::Docs).has_value());
    return type;
}

FieldType body_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

/// A version of `id` with the given body and title; the soft deletes field is `0`,
/// i.e. the document is live.
Document version(std::string id, std::string body, std::string title) {
    Document doc;
    doc.add(std::make_unique<Field>("id", FieldValue{std::move(id)}, id_type()));
    doc.add(std::make_unique<Field>("body", FieldValue{std::move(body)}, body_type()));
    doc.add(std::make_unique<Field>("title", FieldValue{std::move(title)}, body_type()));
    auto sd = NumericDocValuesField::create(std::string(kSoftDeletesField), 0);
    EXPECT_TRUE(sd.has_value());
    doc.add(std::make_unique<Field>(std::move(sd.value())));
    return doc;
}

/// The stored titles the reader shows, sorted.
std::vector<std::string> titles(const std::shared_ptr<DirectoryReader> &reader) {
    IndexSearcher searcher(reader);
    auto hits = searcher.search(MatchAllDocsQuery(), 10);
    EXPECT_TRUE(hits.has_value());
    std::vector<std::string> out;
    auto stored = reader->stored_fields();
    EXPECT_TRUE(stored.has_value());
    for (const ScoreDoc &hit : hits.value().score_docs) {
        auto document = stored.value()->document(hit.doc);
        EXPECT_TRUE(document.has_value());
        auto values = document.value().get_values("title");
        EXPECT_TRUE(values.has_value());
        out.push_back(values.value().empty() ? std::string{} : values.value().front());
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// Writes `a` (body "keep") and `b` (body "other"), then soft updates both.
std::shared_ptr<DirectoryReader> build_index(Directory &directory, std::shared_ptr<const Query> retention_query) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_soft_deletes_field(std::string(kSoftDeletesField));
    config.set_soft_deletes_retention_query(std::move(retention_query));
    config.set_merge_policy(NoMergePolicy::create());
    auto writer = IndexWriter::open(directory, config);
    EXPECT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());
    if (!writer.has_value()) {
        return nullptr;
    }
    EXPECT_TRUE(writer.value()->add_document(version("a", std::string(kKeepTerm), "a-v1")).has_value());
    EXPECT_TRUE(writer.value()->add_document(version("b", "other", "b-v1")).has_value());
    EXPECT_TRUE(writer.value()->commit().has_value());
    EXPECT_TRUE(writer.value()
                        ->soft_update_document(Term::from_text("id", "a"),
                                               version("a", std::string(kKeepTerm), "a-v2"))
                        .has_value());
    EXPECT_TRUE(writer.value()->soft_update_document(Term::from_text("id", "b"), version("b", "other", "b-v2"))
                        .has_value());
    EXPECT_TRUE(writer.value()->commit().has_value());
    auto reader = DirectoryReader::open(directory);
    EXPECT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    if (!reader.has_value()) {
        return nullptr;
    }
    return reader.value();
}

} // namespace

TEST(SoftDeletesRetentionTest, WithoutARetentionQueryBothOldVersionsAreHidden) {
    ByteBuffersDirectory directory;
    auto reader = build_index(directory, nullptr);
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(reader->num_docs().value(), 2);
    EXPECT_EQ(titles(reader), (std::vector<std::string>{"a-v2", "b-v2"}));
}

TEST(SoftDeletesRetentionTest, ARetainedVersionStaysLiveAndKeepsItsData) {
    ByteBuffersDirectory directory;
    auto keep = std::make_shared<TermQuery>(Term::from_text("body", std::string(kKeepTerm)));
    auto reader = build_index(directory, keep);
    ASSERT_NE(reader, nullptr);

    // `a-v1` matches the retention query, so it is not deleted at all; `b-v1` is.
    EXPECT_EQ(reader->num_docs().value(), 3);
    EXPECT_EQ(titles(reader), (std::vector<std::string>{"a-v1", "a-v2", "b-v2"}));

    // Both versions of `a` are found by their body, and only they.
    IndexSearcher searcher(reader);
    auto hits = searcher.search(TermQuery(Term::from_text("body", std::string(kKeepTerm))), 10);
    ASSERT_TRUE(hits.has_value()) << hits.error().to_string();
    EXPECT_EQ(hits.value().total_hits.value, 2U);

    // Nothing was counted as a soft delete for the retained document.
    EXPECT_EQ(reader->leaves().value().front().reader()->num_deleted_docs().value(), 1);
}

TEST(SoftDeletesRetentionTest, ARetentionQueryThatMatchesNothingKeepsTheDefaultBehaviour) {
    ByteBuffersDirectory directory;
    auto none = std::make_shared<TermQuery>(Term::from_text("body", "no-such-term"));
    auto reader = build_index(directory, none);
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(reader->num_docs().value(), 2);
    EXPECT_EQ(titles(reader), (std::vector<std::string>{"a-v2", "b-v2"}));
}

TEST(SoftDeletesRetentionTest, TheRetentionQuerySurvivesACommitAndAMerge) {
    ByteBuffersDirectory directory;
    auto keep = std::make_shared<TermQuery>(Term::from_text("body", std::string(kKeepTerm)));
    {
        auto reader = build_index(directory, keep);
        ASSERT_NE(reader, nullptr);
        EXPECT_EQ(reader->num_docs().value(), 3);
    }

    // A merge has to keep the retained version too, so reopening the index and
    // merging it down to one segment leaves the same documents.
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_soft_deletes_field(std::string(kSoftDeletesField));
    config.set_soft_deletes_retention_query(keep);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());
    auto merged = writer.value()->force_merge(1);
    ASSERT_TRUE(merged.has_value()) << (merged.has_value() ? "" : merged.error().to_string());
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->num_docs().value(), 3) << "the merge must not drop the retained version";
    EXPECT_EQ(titles(reader.value()), (std::vector<std::string>{"a-v1", "a-v2", "b-v2"}));
}
