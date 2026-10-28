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
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.index_writer_lock_policy;

import lucene_core;

using namespace lucene;

namespace {

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

Document make_doc(std::string id) {
    Document doc;
    doc.add(std::make_unique<Field>("id", FieldValue{std::move(id)}, text_type()));
    return doc;
}

/// Whether the directory holds a file whose name starts with `prefix`.
bool has_file(Directory &directory, std::string_view prefix) {
    auto listed = directory.list_all();
    if (!listed.has_value()) {
        return false;
    }
    for (const std::string &file : listed.value()) {
        if (file.starts_with(prefix)) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST(IndexWriterLockTest, ASecondWriterOnTheSameDirectoryIsRejected) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);

    auto first = IndexWriter::open(directory, config);
    ASSERT_TRUE(first.has_value()) << (first ? "" : first.error().to_string());

    auto second = IndexWriter::open(directory, config);
    ASSERT_FALSE(second.has_value()) << "one writer per index";
    EXPECT_EQ(second.error().kind(), ErrorKind::kLockObtainFailed);

    ASSERT_TRUE(first.value()->close().has_value());

    // Once the first writer released the lock the directory is free again.
    auto third = IndexWriter::open(directory, config);
    ASSERT_TRUE(third.has_value()) << (third ? "" : third.error().to_string());
    ASSERT_TRUE(third.value()->close().has_value());
}

TEST(IndexWriterLockTest, DroppingAWriterReleasesTheLock) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);

    {
        auto writer = IndexWriter::open(directory, config);
        ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
        // No `close()`: destroying the writer has to release the lock anyway.
    }

    auto reopened = IndexWriter::open(directory, config);
    ASSERT_TRUE(reopened.has_value()) << (reopened ? "" : reopened.error().to_string());
    ASSERT_TRUE(reopened.value()->close().has_value());
}

TEST(IndexWriterLockTest, AFileSystemWriterKeepsTheLockFileAndReleasesTheLock) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "clucene_writer_lock";
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);

    auto directory = FSDirectory::open(path);
    ASSERT_TRUE(directory.has_value()) << (directory ? "" : directory.error().to_string());
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);

    auto writer = IndexWriter::open(*directory.value(), config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    ASSERT_TRUE(writer.value()->add_document(make_doc("1")).has_value());
    // While the writer is open another one is refused, in this process and in any
    // other one (the lock is an `fcntl` lock on `write.lock.lck`).
    auto competing = IndexWriter::open(*directory.value(), config);
    EXPECT_FALSE(competing.has_value());
    ASSERT_TRUE(writer.value()->close().has_value());

    auto reopened = IndexWriter::open(*directory.value(), config);
    ASSERT_TRUE(reopened.has_value()) << (reopened ? "" : reopened.error().to_string());
    ASSERT_TRUE(reopened.value()->close().has_value());
    // The lock *file* stays around (it is reused); the lock itself was released.
    EXPECT_TRUE(has_file(*directory.value(), "write.lock"));

    std::filesystem::remove_all(path, ignored);
}

TEST(IndexWriterDeletionPolicyTest, TheDefaultPolicyKeepsOnlyTheLastCommit) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    EXPECT_EQ(config.index_deletion_policy()->to_string(), "KeepOnlyLastCommitDeletionPolicy");

    ByteBuffersDirectory directory;
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    ASSERT_TRUE(writer.value()->add_document(make_doc("1")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("2")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());

    auto commits = DirectoryReader::list_commits(directory);
    ASSERT_TRUE(commits.has_value()) << (commits ? "" : commits.error().to_string());
    EXPECT_EQ(commits.value().size(), 1U) << "a commit drops the ones before it";
}

TEST(IndexWriterDeletionPolicyTest, NoDeletionPolicyKeepsEveryCommitAndItsFiles) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_index_deletion_policy(std::make_shared<NoDeletionPolicy>());
    config.set_merge_policy(NoMergePolicy::create());

    ByteBuffersDirectory directory;
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    ASSERT_TRUE(writer.value()->add_document(make_doc("1")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("2")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    // `close()` commits as well (it wrote nothing new, but the policy still sees a
    // commit), so the count is taken before it and the older commits are what
    // matters here.
    auto commits = DirectoryReader::list_commits(directory);
    ASSERT_TRUE(commits.has_value()) << (commits ? "" : commits.error().to_string());
    ASSERT_EQ(commits.value().size(), 2U);

    const int64_t oldest_generation = commits.value().front()->generation();
    const std::vector<std::string> oldest_files = commits.value().front()->file_names();
    auto oldest_reader = DirectoryReader::open_commit(*commits.value().front());
    ASSERT_TRUE(oldest_reader.has_value()) << (oldest_reader ? "" : oldest_reader.error().to_string());
    EXPECT_EQ(oldest_reader.value()->num_docs().value(), 1);

    ASSERT_TRUE(writer.value()->close().has_value());

    // Every file of the older commit is still there, so it can still be opened.
    for (const std::string &file : oldest_files) {
        auto length = directory.file_length(file);
        EXPECT_TRUE(length.has_value()) << file << " must still exist";
    }

    auto after_close = DirectoryReader::list_commits(directory);
    ASSERT_TRUE(after_close.has_value());
    bool kept = false;
    for (const std::shared_ptr<IndexCommit> &commit : after_close.value()) {
        kept = kept || commit->generation() == oldest_generation;
    }
    EXPECT_TRUE(kept) << "NoDeletionPolicy keeps the commit the writer started from";

    // Reopening with the same policy keeps them too.
    auto reopened = IndexWriter::open(directory, config);
    ASSERT_TRUE(reopened.has_value()) << (reopened ? "" : reopened.error().to_string());
    ASSERT_TRUE(reopened.value()->close().has_value());
    auto still_kept = DirectoryReader::list_commits(directory);
    ASSERT_TRUE(still_kept.has_value());
    bool kept_after_reopen = false;
    for (const std::shared_ptr<IndexCommit> &commit : still_kept.value()) {
        kept_after_reopen = kept_after_reopen || commit->generation() == oldest_generation;
    }
    EXPECT_TRUE(kept_after_reopen);
}

TEST(IndexWriterDeletionPolicyTest, ThePolicyDropsTheFilesTheOlderCommitOwned) {
    ByteBuffersDirectory directory;
    std::string first_segment;
    {
        WhitespaceAnalyzer analyzer;
        IndexWriterConfig config(analyzer);
        config.set_merge_policy(NoMergePolicy::create());
        auto writer = IndexWriter::open(directory, config);
        ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
        ASSERT_TRUE(writer.value()->add_document(make_doc("1")).has_value());
        ASSERT_TRUE(writer.value()->commit().has_value());
        ASSERT_TRUE(writer.value()->add_document(make_doc("2")).has_value());
        ASSERT_TRUE(writer.value()->commit().has_value());
        first_segment = writer.value()->segment_infos().segments.front().info.name;
        ASSERT_TRUE(writer.value()->close().has_value());
    }
    EXPECT_TRUE(has_file(directory, first_segment + ".")) << "the first segment is on disk";

    // A merge replaces both segments with one: after the commit that publishes it
    // no commit references the old segments any more, so their files go.
    {
        WhitespaceAnalyzer analyzer;
        IndexWriterConfig config(analyzer);
        auto writer = IndexWriter::open(directory, config);
        ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
        auto merged = writer.value()->force_merge(1);
        ASSERT_TRUE(merged.has_value()) << (merged ? "" : merged.error().to_string());

        const std::vector<SegmentCommitInfo> &segments = writer.value()->segment_infos().segments;
        ASSERT_EQ(segments.size(), 1U);
        EXPECT_NE(segments[0].info.name, first_segment);
        ASSERT_TRUE(writer.value()->close().has_value());
    }
    // The writer held the files of the segment it started from until it closed;
    // releasing the last checkpoint lets them go.
    EXPECT_FALSE(has_file(directory, first_segment + "."))
            << "the commit that dropped the old commit point also deleted its files";

    // The merged documents are all still readable.
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->num_docs().value(), 2);
}
