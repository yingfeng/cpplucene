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
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

module lucene_core:ut.compound_segment;

import lucene_core;

using namespace lucene;

namespace {

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

Document make_doc(std::string id, std::string body) {
    Document doc;
    doc.add(std::make_unique<Field>("id", FieldValue{std::move(id)}, text_type()));
    doc.add(std::make_unique<Field>("body", FieldValue{std::move(body)}, text_type()));
    return doc;
}

Document make_doc_with_value(std::string id, int64_t value) {
    Document doc = make_doc(std::move(id), "hello");
    FieldType values_type;
    EXPECT_TRUE(values_type.set_doc_values_type(DocValuesType::Numeric).has_value());
    doc.add(std::make_unique<Field>("n", FieldValue{value}, values_type));
    return doc;
}

bool any_ends_with(const std::vector<std::string> &names, std::string_view suffix) {
    return std::ranges::any_of(names, [suffix](const std::string &name) { return name.ends_with(suffix); });
}

bool contains(const std::vector<std::string> &names, std::string_view wanted) {
    return std::ranges::any_of(names, [wanted](const std::string &name) { return name == wanted; });
}

} // namespace

TEST(CompoundSegmentTest, AFlushedSegmentIsPackedByDefault) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    EXPECT_TRUE(config.use_compound_file()) << "packing is the default";

    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    ASSERT_TRUE(writer.value()->add_document(make_doc("a", "alpha")).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("b", "beta")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    const std::vector<SegmentCommitInfo> &segments = writer.value()->segment_infos().segments;
    ASSERT_EQ(segments.size(), 1U);
    EXPECT_TRUE(segments[0].info.use_compound_file);
    ASSERT_TRUE(writer.value()->close().has_value());

    auto names = directory.list_all();
    ASSERT_TRUE(names.has_value());
    // Two files per segment instead of the ten a Lucene101 segment writes.
    EXPECT_TRUE(any_ends_with(names.value(), ".cfs"));
    EXPECT_TRUE(any_ends_with(names.value(), ".cfe"));
    EXPECT_FALSE(any_ends_with(names.value(), ".fnm")) << "the field infos are inside the compound";
    EXPECT_FALSE(any_ends_with(names.value(), ".tim"));

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->num_docs().value(), 2);
    // The packed files are transparent to the readers.
    IndexSearcher searcher(reader.value());
    auto hits = searcher.search(TermQuery(Term::from_text("body", "beta")), 10);
    ASSERT_TRUE(hits.has_value());
    EXPECT_EQ(hits.value().score_docs.size(), 1U);

    CheckIndex check_index(directory);
    auto status = check_index.check();
    ASSERT_TRUE(status.has_value()) << (status ? "" : status.error().to_string());
    EXPECT_TRUE(status.value().clean()) << status.value().to_string();
}

TEST(CompoundSegmentTest, TheConfigCanTurnPackingOff) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_use_compound_file(false);

    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    ASSERT_TRUE(writer.value()->add_document(make_doc("a", "alpha")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_FALSE(writer.value()->segment_infos().segments[0].info.use_compound_file);
    ASSERT_TRUE(writer.value()->close().has_value());

    auto names = directory.list_all();
    ASSERT_TRUE(names.has_value());
    EXPECT_FALSE(any_ends_with(names.value(), ".cfs"));
    EXPECT_TRUE(any_ends_with(names.value(), ".fnm"));

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->num_docs().value(), 1);
}

TEST(CompoundSegmentTest, AnInPlaceUpdateStaysOutsideTheCompound) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);

    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    ASSERT_TRUE(writer.value()->add_document(make_doc_with_value("a", 7)).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    // The segment is packed now; the update writes the next generation of the
    // field infos and of the doc values next to it, never into the compound.
    auto updated = writer.value()->update_numeric_doc_value(Term::from_text("id", "a"), "n", 42);
    ASSERT_TRUE(updated.has_value()) << (updated ? "" : updated.error().to_string());
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());

    auto names = directory.list_all();
    ASSERT_TRUE(names.has_value());
    EXPECT_TRUE(contains(names.value(), "_0_1.fnm")) << "the updated field infos are a plain file";
    EXPECT_TRUE(any_ends_with(names.value(), "_1_Lucene90_0.dvd")) << "so are the updated doc values";
    EXPECT_TRUE(any_ends_with(names.value(), "_1_Lucene90_0.dvm"));
    EXPECT_FALSE(contains(names.value(), "_0.fnm")) << "the base generation is inside the compound";

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    const LeafReaderContext &leaf = reader.value()->leaves().value().front();
    auto numbers = leaf.reader()->get_numeric_doc_values("n");
    ASSERT_TRUE(numbers.has_value());
    ASSERT_NE(numbers.value(), nullptr);
    ASSERT_EQ(numbers.value()->next_doc().value(), 0);
    EXPECT_EQ(numbers.value()->long_value().value(), 42);
}

TEST(CompoundSegmentTest, DeletionsAndMergesWorkOnPackedSegments) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_merge_policy(NoMergePolicy::create());

    std::string first_segment;
    {
        auto writer = IndexWriter::open(directory, config);
        ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
        ASSERT_TRUE(writer.value()->add_document(make_doc("a", "alpha")).has_value());
        ASSERT_TRUE(writer.value()->commit().has_value());
        ASSERT_TRUE(writer.value()->add_document(make_doc("b", "beta")).has_value());
        ASSERT_TRUE(writer.value()->commit().has_value());
        first_segment = writer.value()->segment_infos().segments.front().info.name;
        // The live docs are written outside the compound file.
        ASSERT_TRUE(writer.value()->delete_documents(Term::from_text("id", "a")).has_value());
        ASSERT_TRUE(writer.value()->commit().has_value());
        ASSERT_TRUE(writer.value()->close().has_value());
    }
    auto names = directory.list_all();
    ASSERT_TRUE(names.has_value());
    EXPECT_TRUE(any_ends_with(names.value(), ".liv"));
    EXPECT_TRUE(any_ends_with(names.value(), ".cfs"));

    {
        WhitespaceAnalyzer merge_analyzer;
        IndexWriterConfig merge_config(merge_analyzer);
        // Ask for packing unconditionally: the merge of the two tiny segments would
        // otherwise be held back by the `noCFSRatio` heuristic, which keeps a merge
        // that is a large part of the index unpacked.
        auto policy = std::shared_ptr<MergePolicy>(TieredMergePolicy::create());
        ASSERT_TRUE(policy->set_no_cfs_ratio(1.0).has_value());
        merge_config.set_merge_policy(std::move(policy));
        auto writer = IndexWriter::open(directory, merge_config);
        ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
        auto merged = writer.value()->force_merge(1);
        ASSERT_TRUE(merged.has_value()) << (merged ? "" : merged.error().to_string());
        const std::vector<SegmentCommitInfo> &segments = writer.value()->segment_infos().segments;
        ASSERT_EQ(segments.size(), 1U);
        EXPECT_TRUE(segments[0].info.use_compound_file) << "a small merge is packed again";
        EXPECT_NE(segments[0].info.name, first_segment);
        ASSERT_TRUE(writer.value()->close().has_value());
    }

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    // `a` was deleted, `b` survives the merge.
    EXPECT_EQ(reader.value()->num_docs().value(), 1);
    IndexSearcher searcher(reader.value());
    auto hits = searcher.search(TermQuery(Term::from_text("body", "beta")), 10);
    ASSERT_TRUE(hits.has_value());
    EXPECT_EQ(hits.value().score_docs.size(), 1U);
}
