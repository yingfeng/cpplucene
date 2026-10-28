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
#include <string_view>
#include <vector>

module lucene_core:ut.index_writer_merge;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kIdField = "id";

FieldType stored_text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::Docs).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

Document make_doc(const std::string &id) {
    Document doc;
    doc.add(std::make_unique<Field>(std::string(kIdField), FieldValue{id}, stored_text_type()));
    return doc;
}

Term id_term(std::string_view value) { return Term(std::string(kIdField), BytesRef::from_string(value)); }

/// The `id` of every live document of the committed index, segment by segment.
std::vector<std::string> committed_ids(Directory &directory) {
    std::vector<std::string> ids;
    auto infos = SegmentInfos::read_latest(directory);
    EXPECT_TRUE(infos.has_value()) << (infos ? "" : infos.error().to_string());
    if (!infos.has_value()) {
        return ids;
    }
    for (const SegmentCommitInfo &commit : infos.value().segments) {
        auto reader = SegmentReader::open(directory, commit, LUCENE_CURRENT_VERSION.major, io_context_default());
        EXPECT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
        if (!reader.has_value()) {
            continue;
        }
        auto stored = reader.value()->stored_fields();
        EXPECT_TRUE(stored.has_value());
        if (!stored.has_value()) {
            continue;
        }
        auto live = reader.value()->live_docs();
        EXPECT_TRUE(live.has_value());
        const Bits *live_docs = live.has_value() ? live.value() : nullptr;
        for (int32_t doc = 0; doc < reader.value()->max_doc().value(); ++doc) {
            if (live_docs != nullptr && !live_docs->get(doc)) {
                continue;
            }
            auto fields = stored.value()->document(doc);
            EXPECT_TRUE(fields.has_value());
            if (!fields.has_value()) {
                continue;
            }
            auto id = fields.value().get(kIdField);
            if (id.has_value() && id.value().has_value()) {
                ids.push_back(id.value().value());
            }
        }
    }
    return ids;
}

/// `true` when `directory` still holds a file that belongs to `segment`.
bool has_files_of(Directory &directory, std::string_view segment) {
    auto names = directory.list_all();
    EXPECT_TRUE(names.has_value());
    if (!names.has_value()) {
        return false;
    }
    for (const std::string &name : names.value()) {
        if (name.size() > segment.size() && name.substr(0, segment.size()) == segment &&
            (name[segment.size()] == '.' || name[segment.size()] == '_')) {
            return true;
        }
    }
    return false;
}

std::shared_ptr<LogDocMergePolicy> log_doc_policy(int32_t merge_factor) {
    auto policy = LogDocMergePolicy::create();
    EXPECT_TRUE(policy->set_merge_factor(merge_factor).has_value());
    // Merge every segment, however small.
    policy->set_min_merge_docs(0);
    return std::shared_ptr<LogDocMergePolicy>(std::move(policy));
}

} // namespace

TEST(IndexWriterMergeTest, MergesOnCommit) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_max_buffered_docs(1);
    config.set_merge_policy(log_doc_policy(3));

    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    for (int32_t i = 0; i < 12; ++i) {
        ASSERT_TRUE(writer.value()->add_document(make_doc("d" + std::to_string(i))).has_value());
    }
    ASSERT_TRUE(writer.value()->commit().has_value());

    // Twelve one-document segments were merged down.
    const size_t num_segments = writer.value()->segment_infos().segments.size();
    EXPECT_LT(num_segments, 12U);
    EXPECT_GT(num_segments, 0U);
    EXPECT_EQ(writer.value()->num_docs().value(), 12);

    // Every document survived the merge, in order.
    const auto ids = committed_ids(dir);
    ASSERT_EQ(ids.size(), 12U);
    for (int32_t i = 0; i < 12; ++i) {
        EXPECT_EQ(ids[static_cast<size_t>(i)], "d" + std::to_string(i));
    }
}

TEST(IndexWriterMergeTest, NoMergeSchedulerPreventsMerges) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_max_buffered_docs(1);
    config.set_merge_policy(log_doc_policy(3));
    config.set_merge_scheduler(NoMergeScheduler::create());

    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    for (int32_t i = 0; i < 6; ++i) {
        ASSERT_TRUE(writer.value()->add_document(make_doc("d" + std::to_string(i))).has_value());
    }
    ASSERT_TRUE(writer.value()->commit().has_value());

    // The policy asked for merges but the scheduler never ran them.
    EXPECT_EQ(writer.value()->segment_infos().segments.size(), 6U);
    EXPECT_EQ(writer.value()->num_docs().value(), 6);
}

TEST(IndexWriterMergeTest, NoMergePolicyKeepsEverySegment) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_max_buffered_docs(1);
    config.set_merge_policy(NoMergePolicy::create());

    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value());
    for (int32_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(writer.value()->add_document(make_doc("d" + std::to_string(i))).has_value());
    }
    ASSERT_TRUE(writer.value()->commit().has_value());
    EXPECT_EQ(writer.value()->segment_infos().segments.size(), 5U);
    EXPECT_EQ(writer.value()->num_docs().value(), 5);
}

TEST(IndexWriterMergeTest, ForceMergeToOneSegment) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_max_buffered_docs(2);
    // A merge factor of ten keeps the three segments apart until the forced merge.
    config.set_merge_policy(log_doc_policy(10));

    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value());
    for (int32_t i = 0; i < 6; ++i) {
        ASSERT_TRUE(writer.value()->add_document(make_doc("d" + std::to_string(i))).has_value());
    }
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_EQ(writer.value()->segment_infos().segments.size(), 3U);
    ASSERT_TRUE(has_files_of(dir, "_0"));

    auto forced = writer.value()->force_merge(1);
    ASSERT_TRUE(forced.has_value()) << (forced ? "" : forced.error().to_string());
    ASSERT_EQ(writer.value()->segment_infos().segments.size(), 1U);
    EXPECT_EQ(writer.value()->num_docs().value(), 6);

    // The merged segment is the one the commit point references.
    const auto ids = committed_ids(dir);
    ASSERT_EQ(ids.size(), 6U);
    for (int32_t i = 0; i < 6; ++i) {
        EXPECT_EQ(ids[static_cast<size_t>(i)], "d" + std::to_string(i));
    }

    // The files of the segments the merge replaced are gone.
    EXPECT_FALSE(has_files_of(dir, "_0"));
    EXPECT_FALSE(has_files_of(dir, "_1"));
    EXPECT_FALSE(has_files_of(dir, "_2"));
}

TEST(IndexWriterMergeTest, MergeExpungesDeletes) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_max_buffered_docs(3);
    config.set_merge_policy(log_doc_policy(10));

    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value());
    for (int32_t i = 0; i < 6; ++i) {
        ASSERT_TRUE(writer.value()->add_document(make_doc("d" + std::to_string(i))).has_value());
    }
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_EQ(writer.value()->segment_infos().segments.size(), 2U);

    ASSERT_TRUE(writer.value()->delete_documents(id_term("d1")).has_value());
    ASSERT_TRUE(writer.value()->delete_documents(id_term("d4")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    EXPECT_EQ(writer.value()->num_docs().value(), 4);
    EXPECT_TRUE(writer.value()->has_deletions().value());

    ASSERT_TRUE(writer.value()->force_merge(1).has_value());
    ASSERT_EQ(writer.value()->segment_infos().segments.size(), 1U);
    // The deleted documents are gone for good, not just marked.
    EXPECT_EQ(writer.value()->num_docs().value(), 4);
    EXPECT_FALSE(writer.value()->has_deletions().value());

    EXPECT_EQ(committed_ids(dir), (std::vector<std::string>{"d0", "d2", "d3", "d5"}));
}

TEST(IndexWriterMergeTest, ForceMergeDeletesRewritesOnlySegmentsWithDeletes) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_max_buffered_docs(2);
    // A merge factor of ten keeps the three segments apart until the forced merge.
    config.set_merge_policy(log_doc_policy(10));

    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value());
    for (int32_t i = 0; i < 6; ++i) {
        ASSERT_TRUE(writer.value()->add_document(make_doc("d" + std::to_string(i))).has_value());
    }
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_EQ(writer.value()->segment_infos().segments.size(), 3U);

    // Only `_1` loses a document; the other two segments stay untouched.
    ASSERT_TRUE(writer.value()->delete_documents(id_term("d2")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->force_merge_deletes().has_value());

    EXPECT_EQ(writer.value()->num_docs().value(), 5);
    EXPECT_FALSE(writer.value()->has_deletions().value());
    EXPECT_EQ(writer.value()->segment_infos().segments.size(), 3U);
    EXPECT_EQ(committed_ids(dir), (std::vector<std::string>{"d0", "d1", "d3", "d4", "d5"}));
}
