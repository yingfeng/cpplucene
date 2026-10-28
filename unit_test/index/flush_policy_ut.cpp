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

// The flush policy decides when the buffered documents become a segment: the
// default answers for the configured thresholds, and a caller can replace it to
// keep everything in one segment or to flush as early as it likes.

module;

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

module lucene_core:ut.flush_policy;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kBody = "body";

FieldType body_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::Docs).has_value());
    return type;
}

/// A policy that answers what a test wants it to answer.
class FixedFlushPolicy final : public FlushPolicy {
public:
    explicit FixedFlushPolicy(bool answer) : answer_(answer) {}

    [[nodiscard]] bool should_flush(const FlushPolicyContext & /*context*/) const override { return answer_; }

private:
    bool answer_{false};
};

/// Adds `count` documents with a distinct body each.
void add_documents(IndexWriter &writer, int32_t count) {
    for (int32_t i = 0; i < count; ++i) {
        Document document;
        document.add(std::make_unique<Field>(std::string(kBody), FieldValue{std::string("t") + std::to_string(i)},
                                            body_type()));
        ASSERT_TRUE(writer.add_document(std::move(document)).has_value());
    }
}

} // namespace

TEST(FlushPolicyTest, TheDefaultPolicyFlushesOnTheDocumentCount) {
    const auto policy = FlushByRamOrCountsPolicy::create();
    FlushPolicyContext context;
    context.max_buffered_docs = 3;

    context.num_docs_in_ram = 2;
    EXPECT_FALSE(policy->should_flush(context));
    context.num_docs_in_ram = 3;
    EXPECT_TRUE(policy->should_flush(context));
    context.num_docs_in_ram = 4;
    EXPECT_TRUE(policy->should_flush(context));

    // A disabled limit never flushes on its own.
    context.max_buffered_docs = -1;
    EXPECT_FALSE(FlushByRamOrCountsPolicy::flushes_on_doc_count(context));
    EXPECT_FALSE(policy->should_flush(context));
}

TEST(FlushPolicyTest, TheDefaultPolicyFlushesOnTheBufferedBytes) {
    const auto policy = FlushByRamOrCountsPolicy::create();
    FlushPolicyContext context;
    context.ram_buffer_size_bytes = 1000;

    context.bytes_used_in_ram = 999;
    EXPECT_FALSE(policy->should_flush(context));
    context.bytes_used_in_ram = 1000;
    EXPECT_TRUE(policy->should_flush(context));

    // The maximum is what a disabled limit looks like.
    context.ram_buffer_size_bytes = std::numeric_limits<int64_t>::max();
    context.bytes_used_in_ram = std::numeric_limits<int64_t>::max();
    EXPECT_FALSE(FlushByRamOrCountsPolicy::flushes_on_ram(context));
    EXPECT_FALSE(policy->should_flush(context));
}

TEST(FlushPolicyTest, TheDocumentCountWinsWhenBothLimitsAreReached) {
    const auto policy = FlushByRamOrCountsPolicy::create();
    FlushPolicyContext context;
    context.max_buffered_docs = 2;
    context.num_docs_in_ram = 2;
    context.ram_buffer_size_bytes = 1000;
    context.bytes_used_in_ram = 1000;
    EXPECT_TRUE(FlushByRamOrCountsPolicy::flushes_on_doc_count(context));
    EXPECT_TRUE(FlushByRamOrCountsPolicy::flushes_on_ram(context));
    EXPECT_TRUE(policy->should_flush(context));
}

TEST(FlushPolicyTest, TheConfigurationCarriesAPolicy) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    EXPECT_NE(config.flush_policy(), nullptr);
    EXPECT_TRUE(dynamic_cast<const FlushByRamOrCountsPolicy *>(config.flush_policy().get()) != nullptr);

    config.set_flush_policy(std::make_shared<FixedFlushPolicy>(true));
    EXPECT_TRUE(dynamic_cast<const FixedFlushPolicy *>(config.flush_policy().get()) != nullptr);

    // A null policy restores the default instead of leaving the writer without one.
    config.set_flush_policy(nullptr);
    EXPECT_TRUE(dynamic_cast<const FlushByRamOrCountsPolicy *>(config.flush_policy().get()) != nullptr);
}

TEST(FlushPolicyTest, APolicyThatNeverFlushesKeepsOneSegment) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_max_buffered_docs(5);
    config.set_merge_policy(NoMergePolicy::create());
    config.set_flush_policy(std::make_shared<FixedFlushPolicy>(false));

    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());
    add_documents(*writer.value(), 20);
    // Twenty documents would have needed four flushes with the default policy.
    EXPECT_EQ(writer.value()->flush_count(), 0);
    auto docs = writer.value()->num_docs();
    ASSERT_TRUE(docs.has_value());
    EXPECT_EQ(docs.value(), 20);

    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->leaves().value().size(), 1U);
    IndexSearcher searcher(reader.value());
    auto counted = searcher.count(MatchAllDocsQuery());
    ASSERT_TRUE(counted.has_value());
    EXPECT_EQ(counted.value(), 20);
}

TEST(FlushPolicyTest, APolicyCanFlushOnEveryDocument) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_merge_policy(NoMergePolicy::create());
    config.set_flush_policy(std::make_shared<FixedFlushPolicy>(true));

    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());
    add_documents(*writer.value(), 5);
    // The first document finds an empty buffer, so the four that follow flush the
    // one before them.
    EXPECT_EQ(writer.value()->flush_count(), 4);

    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    EXPECT_EQ(reader.value()->leaves().value().size(), 5U);
    IndexSearcher searcher(reader.value());
    auto counted = searcher.count(MatchAllDocsQuery());
    ASSERT_TRUE(counted.has_value());
    EXPECT_EQ(counted.value(), 5);
}

TEST(FlushPolicyTest, TheDefaultPolicyStillSplitsSegmentsAtTheConfiguredCount) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_max_buffered_docs(2);
    config.set_merge_policy(NoMergePolicy::create());

    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());
    add_documents(*writer.value(), 10);
    // Four flushes happen while adding (before the third, fifth, seventh and ninth
    // document); the last two documents are still buffered and go out at commit.
    EXPECT_EQ(writer.value()->flush_count(), 4);

    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    EXPECT_EQ(reader.value()->leaves().value().size(), 5U);
}
