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

#include <memory>
#include <string>

module lucene_core:ut.pending_deletes;

import lucene_core;

using namespace lucene;

namespace {

constexpr int32_t kIdLength = 16;

/// A commit of a segment that already has `del_count` deletions recorded.
SegmentCommitInfo segment_with(int32_t max_doc, int32_t del_count, int64_t del_gen) {
    SegmentCommitInfo segment;
    segment.info.name = "_0";
    segment.info.id.assign(kIdLength, 0x7F);
    segment.info.version = Version{10, 1, 0};
    segment.info.min_version = Version{10, 0, 0};
    segment.info.max_doc = max_doc;
    segment.del_count = del_count;
    segment.del_gen = del_gen;
    return segment;
}

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::Docs).has_value());
    return type;
}

Document make_document(const std::string &body) {
    Document doc;
    doc.add(std::make_unique<Field>(std::string("body"), FieldValue{body}, text_type()));
    return doc;
}

} // namespace

TEST(PendingDeletesTest, StartsOutLiveWithoutABitSet) {
    SegmentCommitInfo segment = segment_with(5, 0, -1);
    const auto &codec = lucene99_codec();
    auto pending = PendingDeletes::create(codec.live_docs_format(), ByteBuffersDirectory(), segment,
                                          io_context_default());
    // `ByteBuffersDirectory` above is a temporary: `create` only reads when the
    // segment has deletions, so the call is valid for this case.
    ASSERT_TRUE(pending.has_value()) << (pending ? "" : pending.error().to_string());
    PendingDeletes &deletes = *pending.value();
    EXPECT_TRUE(deletes.live_docs_initialized());
    EXPECT_EQ(deletes.live_docs(), nullptr);
    EXPECT_EQ(deletes.num_pending_deletes(), 0);
    EXPECT_EQ(deletes.num_live_docs(), 5);
    EXPECT_EQ(deletes.max_doc(), 5);
    EXPECT_EQ(deletes.segment_name(), "_0");
    EXPECT_FALSE(deletes.is_deleted(3).value());
}

TEST(PendingDeletesTest, CountsOnlyTheFirstDeleteOfADocument) {
    auto pending = PendingDeletes::with("_1", std::nullopt, true, 4);
    ASSERT_TRUE(pending.has_value());
    PendingDeletes &deletes = *pending.value();

    auto first = deletes.delete_doc(2);
    ASSERT_TRUE(first.has_value());
    EXPECT_TRUE(first.value());
    EXPECT_EQ(deletes.num_pending_deletes(), 1);
    EXPECT_EQ(deletes.num_live_docs(), 3);
    ASSERT_NE(deletes.live_docs(), nullptr);
    EXPECT_TRUE(deletes.is_deleted(2).value());

    // Deleting the same document again is a no-op.
    auto second = deletes.delete_doc(2);
    ASSERT_TRUE(second.has_value());
    EXPECT_FALSE(second.value());
    EXPECT_EQ(deletes.num_pending_deletes(), 1);

    // Out of bounds documents are rejected.
    EXPECT_FALSE(deletes.delete_doc(4).has_value());
    EXPECT_FALSE(deletes.delete_doc(-1).has_value());

    // `drop_changes` forgets the count but keeps the bits.
    deletes.drop_changes();
    EXPECT_EQ(deletes.num_pending_deletes(), 0);
    EXPECT_EQ(deletes.num_live_docs(), 3);
    EXPECT_TRUE(deletes.is_deleted(2).value());
}

TEST(PendingDeletesTest, RefusesToDeleteBeforeTheLiveDocsAreKnown) {
    // A segment with deletions whose bit set has not been read yet.
    auto pending = PendingDeletes::with("_2", std::nullopt, false, 3);
    ASSERT_TRUE(pending.has_value());
    PendingDeletes &deletes = *pending.value();
    EXPECT_FALSE(deletes.live_docs_initialized());

    auto rejected = deletes.delete_doc(0);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().kind(), ErrorKind::kIllegalState);
}

TEST(PendingDeletesTest, DeletesEveryDocument) {
    auto pending = PendingDeletes::with("_3", std::nullopt, true, 6);
    ASSERT_TRUE(pending.has_value());
    PendingDeletes &deletes = *pending.value();
    deletes.delete_all();
    EXPECT_EQ(deletes.num_live_docs(), 0);
    EXPECT_EQ(deletes.num_pending_deletes(), 6);
    EXPECT_TRUE(deletes.is_deleted(5).value());
}

TEST(PendingDeletesTest, WritesAndReadsBackTheLiveDocs) {
    ByteBuffersDirectory dir;
    SegmentCommitInfo segment = segment_with(5, 0, -1);
    const auto &codec = lucene99_codec();

    auto pending = PendingDeletes::create(codec.live_docs_format(), dir, segment, io_context_default());
    ASSERT_TRUE(pending.has_value());
    PendingDeletes &deletes = *pending.value();
    ASSERT_TRUE(deletes.delete_doc(1).has_value());
    ASSERT_TRUE(deletes.delete_doc(3).has_value());

    auto written = deletes.write_live_docs(codec.live_docs_format(), dir, segment, 2, io_context_default());
    ASSERT_TRUE(written.has_value());
    EXPECT_TRUE(written.value());

    // Recording the new generation is the caller's job, exactly like
    // `SegmentInfos` does for a real commit.
    segment.del_gen = segment.next_del_gen();
    segment.del_count = 2;
    auto read_back = codec.live_docs_format().read_live_docs(dir, segment, io_context_default());
    ASSERT_TRUE(read_back.has_value());
    const FixedBitSet &bits = read_back.value();
    EXPECT_EQ(bits.length(), 5U);
    EXPECT_TRUE(bits.get_bit(0));
    EXPECT_FALSE(bits.get_bit(1));
    EXPECT_TRUE(bits.get_bit(2));
    EXPECT_FALSE(bits.get_bit(3));
    EXPECT_TRUE(bits.get_bit(4));

    // A packet without pending deletions writes nothing.
    deletes.drop_changes();
    auto nothing = deletes.write_live_docs(codec.live_docs_format(), dir, segment, 0, io_context_default());
    ASSERT_TRUE(nothing.has_value());
    EXPECT_FALSE(nothing.value());
}

TEST(PendingDeletesTest, ReadsTheCommittedLiveDocsAndAdoptsAReader) {
    // Build a one segment index with three documents and delete one of them.
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    // Without a merge policy the commit keeps the segment, so the deletion stays
    // in its `.liv` instead of being reclaimed by a single-segment merge.
    config.set_merge_policy(std::make_unique<NoMergePolicy>());
    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    for (const std::string &body : {"alpha", "beta", "gamma"}) {
        ASSERT_TRUE(writer.value()->add_document(make_document(body)).has_value());
    }
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->delete_documents(Term::from_text("body", "beta")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    const SegmentInfos &infos = writer.value()->segment_infos();
    ASSERT_EQ(infos.segments.size(), 1U);
    const SegmentCommitInfo &segment = infos.segments.front();
    ASSERT_TRUE(segment.has_deletions());

    const auto &codec = lucene99_codec();
    auto reopened = PendingDeletes::create(codec.live_docs_format(), dir, segment, io_context_default());
    ASSERT_TRUE(reopened.has_value());
    EXPECT_TRUE(reopened.value()->live_docs_initialized());
    ASSERT_NE(reopened.value()->live_docs(), nullptr);
    EXPECT_EQ(reopened.value()->num_live_docs(), 2);
    EXPECT_EQ(reopened.value()->num_pending_deletes(), 0);

    // The same state can be adopted from an open reader.
    auto reader = SegmentReader::open(dir, segment, LUCENE_CURRENT_VERSION.major, io_context_default());
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    auto from_reader = PendingDeletes::from_reader(*reader.value(), segment);
    ASSERT_TRUE(from_reader.has_value()) << (from_reader ? "" : from_reader.error().to_string());
    EXPECT_EQ(from_reader.value()->num_live_docs(), 2);
    EXPECT_EQ(from_reader.value()->num_pending_deletes(), 0);
    EXPECT_TRUE(from_reader.value()->is_deleted(1).value());

    // An uninitialized instance adopts the reader's live docs.
    auto uninitialized = PendingDeletes::with(std::string(segment.info.name), std::nullopt, false,
                                              static_cast<int32_t>(segment.max_doc()));
    ASSERT_TRUE(uninitialized.has_value());
    EXPECT_FALSE(uninitialized.value()->live_docs_initialized());
    ASSERT_TRUE(uninitialized.value()->on_new_reader(*reader.value()).has_value());
    EXPECT_TRUE(uninitialized.value()->live_docs_initialized());
    ASSERT_NE(uninitialized.value()->live_docs(), nullptr);
    EXPECT_EQ(uninitialized.value()->num_live_docs(), 2);

    ASSERT_TRUE(reader.value()->close().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());
}
