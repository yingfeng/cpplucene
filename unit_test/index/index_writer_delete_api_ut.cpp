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
#include <vector>

module lucene_core:ut.index_writer_delete_api;

import lucene_core;

using namespace lucene;

namespace {

FieldType id_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::Docs).has_value());
    return type;
}

FieldType title_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    return type;
}

Document make_doc(std::string id, std::string title) {
    Document doc;
    doc.add(std::make_unique<Field>("id", FieldValue{std::move(id)}, id_type()));
    doc.add(std::make_unique<Field>("title", FieldValue{std::move(title)}, title_type()));
    return doc;
}

/// The config holds the analyzer by reference, so it has to outlive the writer.
std::shared_ptr<IndexWriter> open_writer(ByteBuffersDirectory &directory, WhitespaceAnalyzer &analyzer) {
    IndexWriterConfig config(analyzer);
    config.set_merge_policy(NoMergePolicy::create());
    auto writer = IndexWriter::open(directory, config);
    EXPECT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    return writer.has_value() ? std::move(writer.value()) : nullptr;
}

int64_t num_docs(Directory &directory) {
    auto reader = DirectoryReader::open(directory);
    EXPECT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    auto count = reader.value()->num_docs();
    EXPECT_TRUE(count.has_value());
    return count.value_or(-1);
}

} // namespace

TEST(IndexWriterDeleteApiTest, DeletesByQuery) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto writer = open_writer(directory, analyzer);
    ASSERT_NE(writer, nullptr);

    ASSERT_TRUE(writer->add_document(make_doc("a", "apple pie")).has_value());
    ASSERT_TRUE(writer->add_document(make_doc("b", "apple tart")).has_value());
    ASSERT_TRUE(writer->add_document(make_doc("c", "banana bread")).has_value());
    {
        auto committed = writer->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }
    ASSERT_EQ(num_docs(directory), 3);

    // Two terms that have to both match: only the apple pie document goes.
    BooleanQuery::Builder builder;
    ASSERT_TRUE(builder.add(std::make_shared<TermQuery>(Term::from_text("title", "apple")), Occur::Must).has_value());
    ASSERT_TRUE(builder.add(std::make_shared<TermQuery>(Term::from_text("title", "pie")), Occur::Must).has_value());
    auto query = builder.build();
    ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());
    ASSERT_TRUE(writer->delete_documents(std::make_shared<const BooleanQuery>(query.value())).has_value());
    EXPECT_TRUE(writer->has_uncommitted_changes());
    {
        auto committed = writer->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }

    EXPECT_EQ(num_docs(directory), 2);
}

TEST(IndexWriterDeleteApiTest, DeletesByEveryQueryOfTheList) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto writer = open_writer(directory, analyzer);
    ASSERT_NE(writer, nullptr);

    ASSERT_TRUE(writer->add_document(make_doc("a", "apple")).has_value());
    ASSERT_TRUE(writer->add_document(make_doc("b", "banana")).has_value());
    ASSERT_TRUE(writer->add_document(make_doc("c", "cherry")).has_value());
    {
        auto committed = writer->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }

    std::vector<std::shared_ptr<const Query>> queries{
            std::make_shared<const TermQuery>(Term::from_text("title", "apple")),
            std::make_shared<const TermQuery>(Term::from_text("title", "cherry"))};
    ASSERT_TRUE(writer->delete_documents(queries).has_value());
    {
        auto committed = writer->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }
    EXPECT_EQ(num_docs(directory), 1);

    auto rejected = writer->delete_documents(std::shared_ptr<const Query>{});
    EXPECT_FALSE(rejected.has_value());
}

TEST(IndexWriterDeleteApiTest, DeletesOneDocumentOfAReader) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto writer = open_writer(directory, analyzer);
    ASSERT_NE(writer, nullptr);

    ASSERT_TRUE(writer->add_document(make_doc("a", "apple")).has_value());
    ASSERT_TRUE(writer->add_document(make_doc("b", "banana")).has_value());
    {
        auto committed = writer->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());

    // The document at 0 is the first one the reader shows.
    auto deleted = writer->try_delete_document(*reader.value(), 0);
    ASSERT_TRUE(deleted.has_value()) << (deleted ? "" : deleted.error().to_string());
    EXPECT_TRUE(deleted.value());
    {
        auto committed = writer->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }
    EXPECT_EQ(num_docs(directory), 1);

    // A document that the reader does not have is refused.
    auto missing = writer->try_delete_document(*reader.value(), 100);
    ASSERT_TRUE(missing.has_value());
    EXPECT_FALSE(missing.value());
    auto negative = writer->try_delete_document(*reader.value(), -1);
    EXPECT_FALSE(negative.has_value());
}

TEST(IndexWriterDeleteApiTest, DeleteUnusedFilesLeavesTheLiveOnes) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto writer = open_writer(directory, analyzer);
    ASSERT_NE(writer, nullptr);

    ASSERT_TRUE(writer->add_document(make_doc("a", "apple")).has_value());
    {
        auto committed = writer->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }

    auto before = directory.list_all();
    ASSERT_TRUE(before.has_value());
    const size_t live_files = before.value().size();

    // Leftovers of a writer that never committed, plus a commit file that has to
    // survive the sweep.
    for (const std::string &name : {std::string("_9.fdt"), std::string("_9_Lucene90_0.dvd")}) {
        auto out = directory.create_output(name, io_context_default());
        ASSERT_TRUE(out.has_value()) << (out ? "" : out.error().to_string());
        const std::vector<uint8_t> payload{1, 2, 3};
        ASSERT_TRUE(out.value()->write_bytes(payload).has_value());
        ASSERT_TRUE(out.value()->close().has_value());
    }

    ASSERT_TRUE(writer->delete_unused_files().has_value());

    auto after = directory.list_all();
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after.value().size(), live_files);
    for (const std::string &name : after.value()) {
        EXPECT_EQ(name.find("_9"), std::string::npos) << name;
    }
    EXPECT_EQ(num_docs(directory), 1);
}

TEST(IndexWriterDeleteApiTest, AClosedWriterRefusesEveryDelete) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto writer = open_writer(directory, analyzer);
    ASSERT_NE(writer, nullptr);
    ASSERT_TRUE(writer->close().has_value());

    EXPECT_FALSE(writer->delete_documents(std::make_shared<const MatchAllDocsQuery>()).has_value());
    EXPECT_FALSE(writer->delete_unused_files().has_value());

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    EXPECT_FALSE(writer->try_delete_document(*reader.value(), 0).has_value());
}

TEST(AddIndexesTest, CopiesEverySegmentOfAnotherIndex) {
    ByteBuffersDirectory source;
    WhitespaceAnalyzer analyzer;
    auto source_writer = open_writer(source, analyzer);
    ASSERT_NE(source_writer, nullptr);
    ASSERT_TRUE(source_writer->add_document(make_doc("a", "apple")).has_value());
    ASSERT_TRUE(source_writer->add_document(make_doc("b", "banana")).has_value());
    {
        auto committed = source_writer->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }
    ASSERT_TRUE(source_writer->close().has_value());

    ByteBuffersDirectory target;
    WhitespaceAnalyzer target_analyzer;
    auto target_writer = open_writer(target, target_analyzer);
    ASSERT_NE(target_writer, nullptr);
    ASSERT_TRUE(target_writer->add_document(make_doc("c", "cherry")).has_value());
    {
        auto committed = target_writer->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }
    ASSERT_EQ(num_docs(target), 1);

    {
        auto added = target_writer->add_indexes_from_directory(source);
        ASSERT_TRUE(added.has_value()) << (added ? "" : added.error().to_string());
    }

    EXPECT_EQ(num_docs(target), 3);
    // The copied segment got a fresh name, so nothing collides with `_0`.
    EXPECT_EQ(target_writer->segment_count(), 2U);

    // And the copied documents are searchable.
    auto reader = DirectoryReader::open(target);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());
    auto hits = searcher.search(TermQuery(Term::from_text("id", "b")), 10);
    ASSERT_TRUE(hits.has_value());
    EXPECT_EQ(hits.value().score_docs.size(), 1U);
}

TEST(AddIndexesTest, CopiesDeletionsAlong) {
    ByteBuffersDirectory source;
    WhitespaceAnalyzer analyzer;
    auto source_writer = open_writer(source, analyzer);
    ASSERT_NE(source_writer, nullptr);
    ASSERT_TRUE(source_writer->add_document(make_doc("a", "apple")).has_value());
    ASSERT_TRUE(source_writer->add_document(make_doc("b", "banana")).has_value());
    {
        auto committed = source_writer->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }
    ASSERT_TRUE(source_writer->delete_documents(Term::from_text("id", "a")).has_value());
    {
        auto committed = source_writer->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }
    ASSERT_TRUE(source_writer->close().has_value());
    ASSERT_EQ(num_docs(source), 1);

    ByteBuffersDirectory target;
    WhitespaceAnalyzer target_analyzer;
    auto target_writer = open_writer(target, target_analyzer);
    ASSERT_NE(target_writer, nullptr);
    {
        auto added = target_writer->add_indexes_from_directory(source);
        ASSERT_TRUE(added.has_value()) << (added ? "" : added.error().to_string());
    }
    EXPECT_EQ(num_docs(target), 1);
}

TEST(AddIndexesTest, RejectsItsOwnDirectory) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto writer = open_writer(directory, analyzer);
    ASSERT_NE(writer, nullptr);
    ASSERT_TRUE(writer->add_document(make_doc("a", "apple")).has_value());
    {
        auto committed = writer->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }

    auto added = writer->add_indexes_from_directory(directory);
    ASSERT_FALSE(added.has_value());
    EXPECT_NE(added.error().to_string().find("itself"), std::string::npos);
}

TEST(AddIndexesTest, CopiesDocValuesUpdatesAndTheFieldInfosGeneration) {
    ByteBuffersDirectory source;
    WhitespaceAnalyzer analyzer;
    auto source_writer = open_writer(source, analyzer);
    ASSERT_NE(source_writer, nullptr);
    Document doc = make_doc("a", "apple");
    auto values = NumericDocValuesField::create("n", 7);
    ASSERT_TRUE(values.has_value());
    doc.add(std::make_unique<Field>(std::move(values.value())));
    ASSERT_TRUE(source_writer->add_document(doc).has_value());
    {
        auto committed = source_writer->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }
    ASSERT_TRUE(source_writer->update_numeric_doc_value(Term::from_text("id", "a"), "n", 42).has_value());
    {
        auto committed = source_writer->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }
    ASSERT_TRUE(source_writer->close().has_value());

    ByteBuffersDirectory target;
    WhitespaceAnalyzer target_analyzer;
    auto target_writer = open_writer(target, target_analyzer);
    ASSERT_NE(target_writer, nullptr);
    {
        auto added = target_writer->add_indexes_from_directory(source);
        ASSERT_TRUE(added.has_value()) << (added ? "" : added.error().to_string());
    }

    auto reader = DirectoryReader::open(target);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    const LeafReaderContext &leaf = reader.value()->leaves().value().front();
    auto numbers = leaf.reader()->get_numeric_doc_values("n");
    ASSERT_TRUE(numbers.has_value());
    ASSERT_NE(numbers.value(), nullptr);
    ASSERT_EQ(numbers.value()->next_doc().value(), 0);
    EXPECT_EQ(numbers.value()->long_value().value(), 42);
}

TEST(IndexWriterApisTest2, UpdatesByQueryAndReportsItsState) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto writer = open_writer(directory, analyzer);
    ASSERT_NE(writer, nullptr);
    ASSERT_TRUE(writer->add_document(make_doc("a", "apple")).has_value());
    {
        auto committed = writer->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }

    const auto names = writer->get_field_names();
    ASSERT_TRUE(names.has_value()) << (names ? "" : names.error().to_string());
    EXPECT_EQ(names.value(), (std::vector<std::string>{"id", "title"}));
    EXPECT_EQ(writer->flush_count(), 1);

    ASSERT_TRUE(writer
                        ->update_document(std::make_shared<const TermQuery>(Term::from_text("id", "a")),
                                          make_doc("a", "apple pie"))
                        .has_value());
    {
        auto committed = writer->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }
    EXPECT_EQ(num_docs(directory), 1);
    EXPECT_EQ(writer->flush_count(), 2);

    // The API around the commit state.
    EXPECT_FALSE(writer->has_pending_merges());
    ASSERT_TRUE(writer->maybe_merge().has_value());
    EXPECT_TRUE(writer->get_live_commit_data().empty());
    EXPECT_EQ(&writer->get_directory(), &directory);
    EXPECT_EQ(writer->get_config().max_buffered_docs(), -1);
}

TEST(RamBufferTest, FlushesWhenTheBufferedBytesExceedTheLimit) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_merge_policy(NoMergePolicy::create());
    // A tiny limit: every handful of documents has to start a new segment.
    config.set_ram_buffer_size_mb(0.0005);  // ~524 bytes
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    EXPECT_GT(writer.value()->get_config().ram_buffer_size_bytes(), 0);
    EXPECT_EQ(writer.value()->get_flushing_bytes(), 0);
    EXPECT_EQ(writer.value()->flush_count(), 0);

    for (int32_t i = 0; i < 40; ++i) {
        ASSERT_TRUE(writer.value()->add_document(make_doc("id" + std::to_string(i), "a title value")).has_value());
    }
    // The limit was reached long before the 40 documents were buffered, so several
    // segments were written and the buffer is small again.
    EXPECT_GT(writer.value()->flush_count(), 1);
    EXPECT_LT(writer.value()->get_flushing_bytes(), writer.value()->get_config().ram_buffer_size_bytes());

    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }
    EXPECT_EQ(num_docs(directory), 40);
    auto pending = writer.value()->get_pending_num_docs();
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(pending.value(), 40);
}

TEST(RamBufferTest, ADocumentCountLimitStillWorks) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_merge_policy(NoMergePolicy::create());
    config.set_max_buffered_docs(4);
    config.set_ram_buffer_size_mb(64.0);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value());

    for (int32_t i = 0; i < 9; ++i) {
        ASSERT_TRUE(writer.value()->add_document(make_doc("id" + std::to_string(i), "text")).has_value());
    }
    // 9 documents in batches of 4: two segments written, one document buffered.
    EXPECT_EQ(writer.value()->flush_count(), 2);
    EXPECT_EQ(writer.value()->num_buffered_documents(), 1);
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }
    EXPECT_EQ(num_docs(directory), 9);
}

/// The leaves of `reader` as codec readers, aliasing the reader that owns them.
std::vector<std::shared_ptr<CodecReader>> codec_readers_of(const std::shared_ptr<DirectoryReader> &reader) {
    std::vector<std::shared_ptr<CodecReader>> readers;
    auto leaves = reader->leaves();
    EXPECT_TRUE(leaves.has_value());
    for (const LeafReaderContext &leaf : leaves.value()) {
        auto *codec = dynamic_cast<CodecReader *>(leaf.reader());
        EXPECT_NE(codec, nullptr);
        if (codec != nullptr) {
            readers.emplace_back(reader, codec);
        }
    }
    return readers;
}

TEST(AddIndexesTest, MergesCodecReadersIntoTheIndex) {
    ByteBuffersDirectory source;
    WhitespaceAnalyzer analyzer;
    auto source_writer = open_writer(source, analyzer);
    ASSERT_NE(source_writer, nullptr);
    ASSERT_TRUE(source_writer->add_document(make_doc("a", "apple")).has_value());
    ASSERT_TRUE(source_writer->add_document(make_doc("b", "banana")).has_value());
    ASSERT_TRUE(source_writer->commit().has_value());
    ASSERT_TRUE(source_writer->close().has_value());

    auto source_reader = DirectoryReader::open(source);
    ASSERT_TRUE(source_reader.has_value());
    auto readers = codec_readers_of(source_reader.value());
    ASSERT_EQ(readers.size(), 1U);

    ByteBuffersDirectory target;
    WhitespaceAnalyzer target_analyzer;
    auto target_writer = open_writer(target, target_analyzer);
    ASSERT_NE(target_writer, nullptr);
    ASSERT_TRUE(target_writer->add_document(make_doc("c", "cherry")).has_value());
    ASSERT_TRUE(target_writer->commit().has_value());
    EXPECT_EQ(num_docs(target), 1);

    auto added = target_writer->add_indexes_from_codec_readers(readers);
    ASSERT_TRUE(added.has_value()) << (added ? "" : added.error().to_string());
    EXPECT_EQ(added.value(), 2);
    EXPECT_EQ(num_docs(target), 3);

    auto reader = DirectoryReader::open(target);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());
    for (const std::string &id : {"a", "b", "c"}) {
        auto hits = searcher.search(TermQuery(Term::from_text("id", id)), 10);
        ASSERT_TRUE(hits.has_value());
        EXPECT_EQ(hits.value().score_docs.size(), 1U) << "id=" << id;
    }

    ASSERT_TRUE(reader.value()->close().has_value());
    ASSERT_TRUE(source_reader.value()->close().has_value());
    ASSERT_TRUE(target_writer->close().has_value());
}

TEST(AddIndexesTest, MergesSeveralReadersIntoOneSegment) {
    ByteBuffersDirectory source;
    WhitespaceAnalyzer analyzer;
    auto source_writer = open_writer(source, analyzer);
    ASSERT_NE(source_writer, nullptr);
    // Two commits produce two segments (the writer uses `NoMergePolicy`).
    ASSERT_TRUE(source_writer->add_document(make_doc("a", "apple")).has_value());
    ASSERT_TRUE(source_writer->commit().has_value());
    ASSERT_TRUE(source_writer->add_document(make_doc("b", "banana")).has_value());
    ASSERT_TRUE(source_writer->commit().has_value());
    ASSERT_TRUE(source_writer->close().has_value());

    auto source_reader = DirectoryReader::open(source);
    ASSERT_TRUE(source_reader.has_value());
    auto readers = codec_readers_of(source_reader.value());
    ASSERT_EQ(readers.size(), 2U);

    ByteBuffersDirectory target;
    WhitespaceAnalyzer target_analyzer;
    auto target_writer = open_writer(target, target_analyzer);
    ASSERT_NE(target_writer, nullptr);

    auto added = target_writer->add_indexes_from_codec_readers(readers);
    ASSERT_TRUE(added.has_value()) << (added ? "" : added.error().to_string());
    EXPECT_EQ(added.value(), 2);
    EXPECT_EQ(num_docs(target), 2);
    EXPECT_EQ(target_writer->segment_infos().segments.size(), 1U);

    // An empty reader list adds nothing and does not commit an extra generation.
    auto nothing = target_writer->add_indexes_from_codec_readers({});
    ASSERT_TRUE(nothing.has_value());
    EXPECT_EQ(nothing.value(), 0);

    ASSERT_TRUE(source_reader.value()->close().has_value());
    ASSERT_TRUE(target_writer->close().has_value());
}
