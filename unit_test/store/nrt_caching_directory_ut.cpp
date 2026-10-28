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
#include <memory>
#include <string>
#include <vector>

module lucene_core:ut.nrt_caching_directory;

import lucene_core;

using namespace lucene;

namespace {

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

Document make_document(const std::string &body) {
    Document doc;
    doc.add(std::make_unique<Field>(std::string("body"), FieldValue{body}, text_type()));
    return doc;
}

/// The delegate directory, plus the cache wrapper on top of it.
struct Fixture {
    explicit Fixture(double max_merge_size_mb, double max_cached_mb) {
        auto delegate = std::make_unique<ByteBuffersDirectory>();
        delegate_raw = delegate.get();
        cached = std::make_unique<NRTCachingDirectory>(std::move(delegate), max_merge_size_mb, max_cached_mb);
    }

    ByteBuffersDirectory *delegate_raw{nullptr};
    std::unique_ptr<NRTCachingDirectory> cached;
};

} // namespace

TEST(NRTCachingDirectoryTest, KeepsFlushedSegmentsInRamUntilCommit) {
    Fixture fixture(1024.0, 1024.0);
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    // These tests follow individual files between the cached and the delegate
    // directory, so the segments must stay unpacked.
    config.set_use_compound_file(false);
    auto writer = IndexWriter::open(*fixture.cached, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    for (const std::string &body : {"alpha", "beta", "gamma"}) {
        ASSERT_TRUE(writer.value()->add_document(make_document(body)).has_value());
    }

    // A near-real-time read flushes the buffered documents; the flush context
    // makes the new segment small enough to cache, so the files stay in RAM.
    auto nrt_reader = writer.value()->get_reader();
    ASSERT_TRUE(nrt_reader.has_value()) << (nrt_reader ? "" : nrt_reader.error().to_string());
    auto cached_bytes = fixture.cached->cached_bytes();
    ASSERT_TRUE(cached_bytes.has_value());
    EXPECT_GT(cached_bytes.value(), 0);
    auto cached_files = fixture.cached->list_cached_files();
    ASSERT_TRUE(cached_files.has_value());
    EXPECT_FALSE(cached_files.value().empty());
    for (const std::string &name : cached_files.value()) {
        // The delegate does not have these files yet.
        EXPECT_FALSE(fixture.delegate_raw->file_length(name).has_value()) << name;
    }

    // The reader sees the documents while they are only in the cache.
    IndexSearcher searcher(nrt_reader.value());
    EXPECT_EQ(searcher.count(MatchAllDocsQuery()).value(), 3);

    // A commit syncs the files, which is where the cache hands them over.
    ASSERT_TRUE(writer.value()->commit().has_value());
    auto after_commit = fixture.cached->cached_bytes();
    ASSERT_TRUE(after_commit.has_value());
    EXPECT_EQ(after_commit.value(), 0);
    auto cached_after = fixture.cached->list_cached_files();
    ASSERT_TRUE(cached_after.has_value());
    EXPECT_TRUE(cached_after.value().empty());
    for (const std::string &name : cached_files.value()) {
        EXPECT_TRUE(fixture.delegate_raw->file_length(name).has_value()) << name;
    }

    ASSERT_TRUE(writer.value()->close().has_value());

    // Reading through the wrapper now hits the delegate.
    auto reader = DirectoryReader::open(*fixture.cached);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    IndexSearcher reopened(reader.value());
    EXPECT_EQ(reopened.count(MatchAllDocsQuery()).value(), 3);
}

TEST(NRTCachingDirectoryTest, WritesStraightThroughWhenCachingIsOff) {
    // Zero merge size: no flush context is small enough, so nothing is cached.
    Fixture fixture(0.0, 1024.0);
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    // These tests follow individual files between the cached and the delegate
    // directory, so the segments must stay unpacked.
    config.set_use_compound_file(false);
    auto writer = IndexWriter::open(*fixture.cached, config);
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer.value()->add_document(make_document("alpha")).has_value());

    auto nrt_reader = writer.value()->get_reader();
    ASSERT_TRUE(nrt_reader.has_value());
    auto cached_bytes = fixture.cached->cached_bytes();
    ASSERT_TRUE(cached_bytes.has_value());
    EXPECT_EQ(cached_bytes.value(), 0);

    auto nrt_files = fixture.cached->list_cached_files();
    ASSERT_TRUE(nrt_files.has_value());
    EXPECT_TRUE(nrt_files.value().empty());

    // The delegate already holds the segment files.
    auto delegate_files = fixture.delegate_raw->list_all();
    ASSERT_TRUE(delegate_files.has_value());
    bool has_segment_file = false;
    for (const std::string &name : delegate_files.value()) {
        if (name.starts_with("_0.")) {
            has_segment_file = true;
        }
    }
    EXPECT_TRUE(has_segment_file);
    ASSERT_TRUE(writer.value()->commit().has_value());
}

TEST(NRTCachingDirectoryTest, ListsBothDirectoriesAndDeletesFromTheRightOne) {
    Fixture fixture(1024.0, 1024.0);
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    // These tests follow individual files between the cached and the delegate
    // directory, so the segments must stay unpacked.
    config.set_use_compound_file(false);
    auto writer = IndexWriter::open(*fixture.cached, config);
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer.value()->add_document(make_document("alpha")).has_value());

    auto nrt_reader = writer.value()->get_reader();
    ASSERT_TRUE(nrt_reader.has_value());
    auto cached_files = fixture.cached->list_cached_files();
    ASSERT_TRUE(cached_files.has_value());
    ASSERT_FALSE(cached_files.value().empty());
    const std::string cached_name = cached_files.value().front();

    auto listed = fixture.cached->list_all();
    ASSERT_TRUE(listed.has_value());
    EXPECT_NE(std::find(listed.value().begin(), listed.value().end(), cached_name), listed.value().end());

    ASSERT_TRUE(fixture.cached->delete_file(cached_name).has_value());
    EXPECT_FALSE(fixture.cached->file_length(cached_name).has_value());
    EXPECT_FALSE(fixture.delegate_raw->file_length(cached_name).has_value());

    // The wrapper still describes itself.
    EXPECT_NE(fixture.cached->to_string().find("NRTCachingDirectory"), std::string::npos);
    ASSERT_TRUE(nrt_reader.value()->close().has_value());
}
