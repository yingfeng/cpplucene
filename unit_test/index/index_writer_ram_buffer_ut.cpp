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
#include <memory>
#include <string>

module lucene_core:ut.index_writer_ram_buffer;

import lucene_core;

using namespace lucene;

namespace {

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    return type;
}

/// A document with roughly `bytes` bytes of indexed text.
Document text_document(int32_t id, size_t words) {
    Document doc;
    std::string body;
    for (size_t i = 0; i < words; ++i) {
        body += "word" + std::to_string(id) + "_" + std::to_string(i) + " ";
    }
    doc.add(std::make_unique<Field>("body", FieldValue{std::move(body)}, text_type()));
    return doc;
}

/// `config` with the given RAM buffer, no document-count trigger and no merges.
void configure(IndexWriterConfig &config, double ram_buffer_size_mb) {
    config.set_max_buffered_docs(-1);
    config.set_ram_buffer_size_mb(ram_buffer_size_mb);
    config.set_merge_policy(NoMergePolicy::create());
}

} // namespace

/// The RAM trigger is what bounds an indexing run's memory use, so it has to
/// flush even when `max_buffered_docs` would never fire.
TEST(IndexWriterRamBufferTest, FlushesWhenTheRamBufferIsFull) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    // 10 KB of buffer against ~100 documents of a few hundred bytes each.
    configure(config, 0.01);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    for (int32_t id = 0; id < 100; ++id) {
        ASSERT_TRUE(writer.value()->add_document(text_document(id, 20)).has_value());
    }
    ASSERT_TRUE(writer.value()->commit().has_value());
    EXPECT_GT(writer.value()->segment_count(), 1U) << "the RAM buffer never filled";
    EXPECT_EQ(writer.value()->num_docs().value(), 100);
    ASSERT_TRUE(writer.value()->close().has_value());

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->num_docs().value(), 100);
    auto searcher = IndexSearcher(reader.value());
    auto hits = searcher.count(MatchAllDocsQuery());
    ASSERT_TRUE(hits.has_value());
    EXPECT_EQ(hits.value(), 100);
}

/// The same documents with a buffer that never fills stay in one segment.
TEST(IndexWriterRamBufferTest, KeepsOneSegmentWhenTheBufferIsLarge) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    configure(config, 1024.0); // 1 GB
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value());

    for (int32_t id = 0; id < 100; ++id) {
        ASSERT_TRUE(writer.value()->add_document(text_document(id, 20)).has_value());
    }
    ASSERT_TRUE(writer.value()->commit().has_value());
    EXPECT_EQ(writer.value()->segment_count(), 1U);
    EXPECT_EQ(writer.value()->num_docs().value(), 100);
    ASSERT_TRUE(writer.value()->close().has_value());
}

/// `max_buffered_docs` still works on its own, and the default RAM buffer does
/// not interfere with it.
TEST(IndexWriterRamBufferTest, DocumentCountTriggerStillWorks) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_max_buffered_docs(10);
    config.set_ram_buffer_size_mb(1024.0);
    config.set_merge_policy(NoMergePolicy::create());
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value());

    for (int32_t id = 0; id < 35; ++id) {
        ASSERT_TRUE(writer.value()->add_document(text_document(id, 1)).has_value());
    }
    ASSERT_TRUE(writer.value()->commit().has_value());
    EXPECT_GE(writer.value()->segment_count(), 3U);
    EXPECT_EQ(writer.value()->num_docs().value(), 35);
    ASSERT_TRUE(writer.value()->close().has_value());
}

/// Disabling both triggers (`-1` docs, `0` MB) keeps everything buffered until
/// the commit, which is what a caller asking for a single segment wants.
TEST(IndexWriterRamBufferTest, BothTriggersCanBeDisabled) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_max_buffered_docs(-1);
    config.set_ram_buffer_size_mb(0.0);
    config.set_merge_policy(NoMergePolicy::create());
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value());

    for (int32_t id = 0; id < 50; ++id) {
        ASSERT_TRUE(writer.value()->add_document(text_document(id, 5)).has_value());
    }
    ASSERT_TRUE(writer.value()->commit().has_value());
    EXPECT_EQ(writer.value()->segment_count(), 1U);
    EXPECT_EQ(writer.value()->num_docs().value(), 50);
    ASSERT_TRUE(writer.value()->close().has_value());
}
