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
#include <vector>

module lucene_core:ut.index_writer_observers;

import lucene_core;

using namespace lucene;

namespace {

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::Docs).has_value());
    return type;
}

Document make_doc(int32_t id) {
    Document doc;
    doc.add(std::make_unique<Field>("id", FieldValue{std::to_string(id)}, text_type()));
    auto price = NumericDocValuesField::create("price", id);
    EXPECT_TRUE(price.has_value());
    doc.add(std::make_unique<Field>(std::move(price.value())));
    return doc;
}

/// Records the merge events it is told about.
class RecordingListener final : public IndexWriterEventListener {
public:
    void begin_merge_on_full_flush(std::span<const MergeStat> merge_states) override {
        ++begins;
        recorded.assign(merge_states.begin(), merge_states.end());
    }

    void end_merge_on_full_flush(std::span<const MergeStat> merge_states) override {
        ++ends;
        ended_with = static_cast<int32_t>(merge_states.size());
    }

    [[nodiscard]] std::string to_string() const override { return "RecordingListener"; }

    int32_t begins{0};
    int32_t ends{0};
    int32_t ended_with{0};
    std::vector<MergeStat> recorded;
};

/// Counts how many segments it was asked to warm.
class CountingWarmer final : public IndexReaderWarmer {
public:
    [[nodiscard]] VoidResult warm(LeafReader & /*reader*/) override {
        ++warmed;
        return {};
    }

    int32_t warmed{0};
};

} // namespace

TEST(IndexWriterEventListenerTest, NoOpListenerIsTheDefault) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    ASSERT_NE(config.event_listener(), nullptr);
    EXPECT_EQ(config.event_listener()->to_string(), "NoOpIndexWriterEventListener");
    // Setting a null listener restores the no-op one instead of leaving a hole.
    config.set_event_listener(nullptr);
    EXPECT_EQ(config.event_listener()->to_string(), "NoOpIndexWriterEventListener");
}

TEST(IndexWriterEventListenerTest, SeesTheMergesOfAForcedMerge) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    // One segment per commit, so the forced merge has several inputs.
    config.set_max_buffered_docs(10);
    auto listener = std::make_shared<RecordingListener>();
    config.set_event_listener(listener);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    for (int32_t id = 0; id < 30; ++id) {
        ASSERT_TRUE(writer.value()->add_document(make_doc(id)).has_value());
        if (id % 10 == 9) {
            ASSERT_TRUE(writer.value()->commit().has_value());
        }
    }
    ASSERT_GT(writer.value()->segment_count(), 1U);

    ASSERT_TRUE(writer.value()->force_merge(1).has_value()) << "force_merge failed";
    EXPECT_EQ(writer.value()->segment_count(), 1U);

    // Every merge batch is announced and closed exactly once.
    EXPECT_GT(listener->begins, 0);
    EXPECT_EQ(listener->begins, listener->ends);
    EXPECT_GT(listener->ended_with, 0);
    ASSERT_FALSE(listener->recorded.empty());
    EXPECT_FALSE(listener->recorded.front().segments.empty());
    EXPECT_FALSE(listener->recorded.front().segments.front().name.empty());
    EXPECT_NE(listener->recorded.front().to_string().find("MergeStat"), std::string::npos);

    ASSERT_TRUE(writer.value()->close().has_value());
}

TEST(SimpleMergedSegmentWarmerTest, WarmsTheMergedSegment) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_max_buffered_docs(5);
    auto warmer = std::make_shared<SimpleMergedSegmentWarmer>();
    config.set_merged_segment_warmer(warmer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value());

    for (int32_t id = 0; id < 20; ++id) {
        ASSERT_TRUE(writer.value()->add_document(make_doc(id)).has_value());
    }
    ASSERT_TRUE(writer.value()->commit().has_value());
    // A forced merge definitely rewrites a segment, so the warmer has to run.
    ASSERT_TRUE(writer.value()->force_merge(1).has_value());
    EXPECT_EQ(writer.value()->segment_count(), 1U);

    // The two fields are one indexed field plus one doc values field.
    ASSERT_GT(writer.value()->segment_count(), 0U);
    EXPECT_GT(warmer->indexed_fields(), 0);
    EXPECT_GT(warmer->doc_values_fields(), 0);
    EXPECT_GE(warmer->norms_fields(), 0);

    ASSERT_TRUE(writer.value()->close().has_value());
}

TEST(SimpleMergedSegmentWarmerTest, AcceptsAnInfoStream) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_max_buffered_docs(5);
    auto warmer = std::make_shared<SimpleMergedSegmentWarmer>(get_default_info_stream());
    config.set_merged_segment_warmer(warmer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value());
    for (int32_t id = 0; id < 10; ++id) {
        ASSERT_TRUE(writer.value()->add_document(make_doc(id)).has_value());
    }
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->force_merge(1).has_value());
    ASSERT_TRUE(writer.value()->close().has_value());
}

TEST(MergeStatTest, FormatsNameAndId) {
    MergeStat stat;
    stat.merge_gen = 7;
    stat.segments.push_back(MergeStat::Segment{"_0", std::vector<uint8_t>{0x0A, 0xFF}});
    stat.segments.push_back(MergeStat::Segment{"_1", std::vector<uint8_t>{}});
    EXPECT_EQ(stat.segments[0].id_hex(), "0aff");
    EXPECT_EQ(stat.segments[1].id_hex(), "");
    EXPECT_EQ(stat.to_string(), "MergeStat(gen=7, segments=[_0, _1])");
}
