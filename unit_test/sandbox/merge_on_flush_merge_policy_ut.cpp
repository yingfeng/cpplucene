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
#include <vector>

module lucene_core:ut.merge_on_flush_merge_policy;

import lucene_core;

using namespace lucene;

namespace {

std::string add_documents(IndexWriter &writer, std::string_view text, int32_t count) {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::Docs).has_value());
    for (int32_t i = 0; i < count; ++i) {
        Document doc;
        doc.add(std::make_unique<Field>("body", FieldValue{std::string(text)}, type));
        EXPECT_TRUE(writer.add_document(std::move(doc)).has_value());
    }
    return std::string(text);
}

} // namespace

TEST(MergeOnFlushMergePolicyTest, ReportsTheDelegateAndThreshold) {
    auto policy = std::make_unique<MergeOnFlushMergePolicy>(std::make_unique<NoMergePolicy>());
    EXPECT_EQ(policy->to_string(), "MergeOnFlushMergePolicy(NoMergePolicy)");
    // The default threshold is 100 MB.
    EXPECT_DOUBLE_EQ(policy->small_segment_threshold_mb(), 100.0);
    policy->set_small_segment_threshold_mb(1.5);
    EXPECT_DOUBLE_EQ(policy->small_segment_threshold_mb(), 1.5);
    EXPECT_EQ(mb_to_bytes(1.0), 1024LL * 1024);
    EXPECT_DOUBLE_EQ(bytes_to_mb(1024LL * 1024), 1.0);

    // NoMergePolicy never asks for a merge, and neither does the wrapper when the
    // trigger is not a full flush: the call is delegated.
    EXPECT_EQ(policy->max_full_flush_merge_size(), 0);
    EXPECT_EQ(policy->delegate().to_string(), "NoMergePolicy");

    // A null delegate falls back to NoMergePolicy instead of crashing.
    auto null_delegate = std::make_unique<MergeOnFlushMergePolicy>(nullptr);
    EXPECT_EQ(null_delegate->delegate().to_string(), "NoMergePolicy");
}

TEST(MergeOnFlushMergePolicyTest, MergesSmallSegmentsOnCommit) {
    auto directory = std::make_shared<ByteBuffersDirectory>();
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);

    auto policy = std::make_shared<MergeOnFlushMergePolicy>(std::make_unique<NoMergePolicy>());
    policy->set_small_segment_threshold_mb(100.0);
    config.set_merge_policy(policy);

    auto writer = IndexWriter::open(*directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());

    // Each commit flushes a new small segment; the policy merges them on the next
    // full flush, so the index never accumulates many tiny segments.
    for (int32_t round = 0; round < 4; ++round) {
        add_documents(*writer.value(), "alpha", 2);
        ASSERT_TRUE(writer.value()->commit().has_value());
    }
    add_documents(*writer.value(), "beta", 2);
    ASSERT_TRUE(writer.value()->commit().has_value());

    EXPECT_LE(writer.value()->segment_count(), 5U);
    ASSERT_TRUE(writer.value()->close().has_value());

    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->num_docs(), 10);
}
