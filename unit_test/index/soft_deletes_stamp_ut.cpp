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

// A soft delete has to leave a trace in the soft deletes field: the writer stamps
// the deleted documents with a non-zero generation, which is what lets a later
// reader tell a soft delete from a hard one (and what a retention policy needs to
// decide when the document may really go).

module;

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.soft_deletes_stamp;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kSoftDeletesField = "sd";

FieldType id_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::Docs).has_value());
    return type;
}

/// A document whose soft deletes field is `0`, i.e. a live version.
Document version(std::string id, std::string title) {
    Document doc;
    doc.add(std::make_unique<Field>("id", FieldValue{std::move(id)}, id_type()));
    auto sd = NumericDocValuesField::create(std::string(kSoftDeletesField), 0);
    EXPECT_TRUE(sd.has_value());
    doc.add(std::make_unique<Field>(std::move(sd.value())));
    auto stored = FieldType();
    EXPECT_TRUE(stored.set_stored(true).has_value());
    doc.add(std::make_unique<Field>("title", FieldValue{std::move(title)}, stored));
    return doc;
}

/// The soft deletes value of `doc` in `reader`, or -1 when it has none.
int64_t soft_delete_value(LeafReader *reader, int32_t doc) {
    auto values = reader->get_numeric_doc_values(std::string(kSoftDeletesField));
    EXPECT_TRUE(values.has_value()) << (values.has_value() ? "" : values.error().to_string());
    if (!values.has_value() || values.value() == nullptr) {
        return -1;
    }
    if (!values.value()->advance_exact(doc).value()) {
        return -1;
    }
    auto value = values.value()->long_value();
    EXPECT_TRUE(value.has_value()) << (value.has_value() ? "" : value.error().to_string());
    return value.has_value() ? value.value() : -1;
}

} // namespace

TEST(SoftDeletesStampTest, TheWriterStampsTheSoftDeletedDocuments) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_soft_deletes_field(std::string(kSoftDeletesField));
    config.set_merge_policy(NoMergePolicy::create());
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());

    ASSERT_TRUE(writer.value()->add_document(version("a", "v1")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->soft_update_document(Term::from_text("id", "a"), version("a", "v2")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->num_docs().value(), 1); // v2 is the visible version

    // The first segment holds only the soft-deleted v1, whose field was 0 when it
    // was indexed: the writer has to have written the generation into it.
    const auto leaves = reader.value()->leaves().value();
    ASSERT_EQ(leaves.size(), 2U);
    LeafReader *old_segment = leaves.front().reader();
    // The exact value is the generation of the commit that applied the delete; all
    // that matters here is that it is set.
    EXPECT_GT(soft_delete_value(old_segment, 0), 0) << "the soft delete has to be stamped";
    LeafReader *new_segment = leaves.back().reader();
    EXPECT_EQ(soft_delete_value(new_segment, 0), 0) << "the live version keeps its own value";
}

TEST(SoftDeletesStampTest, AFullyDeletedSegmentCanBeHiddenByTheField) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_soft_deletes_field(std::string(kSoftDeletesField));
    config.set_merge_policy(NoMergePolicy::create());
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value());

    ASSERT_TRUE(writer.value()->add_document(version("a", "v1")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->soft_update_document(Term::from_text("id", "a"), version("a", "v2")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    auto *codec_reader = dynamic_cast<CodecReader *>(reader.value()->leaves().value().front().reader());
    ASSERT_NE(codec_reader, nullptr) << "the leaf has to be a codec reader";

    // Filtering by the field alone hides the document, which is what a retention
    // policy relies on when it decides to keep soft-deleted data around.
    std::shared_ptr<CodecReader> shared(codec_reader, [](CodecReader *) {});
    auto wrapped = wrap_soft_deletes(shared, kSoftDeletesField);
    ASSERT_TRUE(wrapped.has_value()) << (wrapped.has_value() ? "" : wrapped.error().to_string());
    ASSERT_NE(wrapped.value(), nullptr);
    EXPECT_EQ(wrapped.value()->num_docs().value(), 0);
}

TEST(SoftDeletesStampTest, AHardDeleteLeavesNoStamp) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_soft_deletes_field(std::string(kSoftDeletesField));
    config.set_merge_policy(NoMergePolicy::create());
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value());

    ASSERT_TRUE(writer.value()->add_document(version("a", "v1")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->delete_documents(Term::from_text("id", "a")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    EXPECT_EQ(reader.value()->num_docs().value(), 0);
    // A hard delete is not part of the soft deletes bookkeeping.
    const SegmentCommitInfo &segment = writer.value()->segment_infos().segments.front();
    EXPECT_EQ(segment.soft_del_count, 0);
    EXPECT_EQ(segment.del_count, 1);
}
