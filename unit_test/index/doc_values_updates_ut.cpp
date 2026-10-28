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
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.doc_values_updates;

import lucene_core;

using namespace lucene;

namespace {

FieldType id_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::Docs).has_value());
    return type;
}

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    return type;
}

std::vector<uint8_t> bytes_of(std::string_view text) {
    return std::vector<uint8_t>(text.begin(), text.end());
}

/// A document identified by `id`, with `value` in a numeric doc values field `n`.
Document numeric_doc(std::string id, int64_t value) {
    Document doc;
    doc.add(std::make_unique<Field>("id", FieldValue{std::move(id)}, id_type()));
    auto field = NumericDocValuesField::create("n", value);
    EXPECT_TRUE(field.has_value()) << (field ? "" : field.error().to_string());
    doc.add(std::make_unique<Field>(std::move(field.value())));
    return doc;
}

/// A document identified by `id`, with `value` in a binary doc values field `b`.
Document binary_doc(std::string id, std::string value) {
    Document doc;
    doc.add(std::make_unique<Field>("id", FieldValue{std::move(id)}, id_type()));
    auto field = BinaryDocValuesField::create("b", bytes_of(value));
    EXPECT_TRUE(field.has_value()) << (field ? "" : field.error().to_string());
    doc.add(std::make_unique<Field>(std::move(field.value())));
    return doc;
}

Term id_term(std::string_view id) {
    return Term::from_text("id", id);
}

/// The value of every document that has one, keyed by the global document number.
std::map<int32_t, int64_t> numeric_values(const std::shared_ptr<DirectoryReader> &reader) {
    std::map<int32_t, int64_t> values;
    for (const LeafReaderContext &leaf : reader->leaves().value()) {
        auto numbers = leaf.reader()->get_numeric_doc_values("n");
        if (!numbers.has_value() || numbers.value() == nullptr) {
            continue;
        }
        auto doc = numbers.value()->next_doc();
        while (doc.has_value() && doc.value() != NO_MORE_DOCS) {
            auto value = numbers.value()->long_value();
            EXPECT_TRUE(value.has_value()) << (value ? "" : value.error().to_string());
            if (value.has_value()) {
                values[leaf.doc_base() + doc.value()] = value.value();
            }
            doc = numbers.value()->next_doc();
        }
    }
    return values;
}

/// The text of `field` of every document, keyed by the global document number.
std::map<int32_t, std::string> binary_values(const std::shared_ptr<DirectoryReader> &reader,
                                            std::string_view field) {
    std::map<int32_t, std::string> values;
    for (const LeafReaderContext &leaf : reader->leaves().value()) {
        auto doc_values = leaf.reader()->get_binary_doc_values(field);
        if (!doc_values.has_value() || doc_values.value() == nullptr) {
            continue;
        }
        auto doc = doc_values.value()->next_doc();
        while (doc.has_value() && doc.value() != NO_MORE_DOCS) {
            auto value = doc_values.value()->binary_value();
            EXPECT_TRUE(value.has_value()) << (value ? "" : value.error().to_string());
            if (value.has_value()) {
                auto text = value.value().utf8_to_string();
                EXPECT_TRUE(text.has_value());
                values[leaf.doc_base() + doc.value()] = text.value_or(std::string{});
            }
            doc = doc_values.value()->next_doc();
        }
    }
    return values;
}

/// The values in ascending order, so a test can compare them without caring
/// which document a merge put first.
std::vector<int64_t> sorted_values(std::map<int32_t, int64_t> values) {
    std::vector<int64_t> out;
    for (const auto &[doc, value] : values) {
        out.push_back(value);
    }
    std::ranges::sort(out);
    return out;
}

} // namespace

TEST(DocValuesUpdatesTest, UpdatesANumericFieldInPlace) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(directory, IndexWriterConfig(analyzer));
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    ASSERT_TRUE(writer.value()->add_document(numeric_doc("a", 10)).has_value());
    ASSERT_TRUE(writer.value()->add_document(numeric_doc("b", 20)).has_value());
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }

    ASSERT_TRUE(writer.value()->update_numeric_doc_value(id_term("b"), "n", 99).has_value());
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    const auto values = numeric_values(reader.value());
    ASSERT_EQ(values.size(), 2U);
    EXPECT_EQ(values.at(0), 10);
    EXPECT_EQ(values.at(1), 99);

    // The update wrote one doc values generation and one field infos generation.
    const SegmentInfos &infos = writer.value()->segment_infos();
    ASSERT_EQ(infos.segments.size(), 1U);
    EXPECT_EQ(infos.segments[0].doc_values_gen, 1);
    EXPECT_EQ(infos.segments[0].field_infos_gen, 1);
    // And the files belong to the segment, so a merge or a delete can find them.
    bool has_generation_file = false;
    for (const std::string &file : infos.segments[0].info.files) {
        if (file.ends_with(".dvd") && file.find("_1_") != std::string::npos) {
            has_generation_file = true;
        }
    }
    EXPECT_TRUE(has_generation_file);
}

TEST(DocValuesUpdatesTest, UpdatesABinaryFieldInPlace) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(directory, IndexWriterConfig(analyzer));
    ASSERT_TRUE(writer.has_value());

    ASSERT_TRUE(writer.value()->add_document(binary_doc("a", "one")).has_value());
    ASSERT_TRUE(writer.value()->add_document(binary_doc("b", "two")).has_value());
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }

    ASSERT_TRUE(writer.value()
                        ->update_binary_doc_value(id_term("a"), "b", BytesRef::from_string("uno"))
                        .has_value());
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    const auto values = binary_values(reader.value(), "b");
    ASSERT_EQ(values.size(), 2U);
    EXPECT_EQ(values.at(0), "uno");
    EXPECT_EQ(values.at(1), "two");
}

TEST(DocValuesUpdatesTest, RemovesAValueAndSurvivesASecondGeneration) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(directory, IndexWriterConfig(analyzer));
    ASSERT_TRUE(writer.has_value());

    ASSERT_TRUE(writer.value()->add_document(numeric_doc("a", 1)).has_value());
    ASSERT_TRUE(writer.value()->add_document(numeric_doc("b", 2)).has_value());
    ASSERT_TRUE(writer.value()->add_document(numeric_doc("c", 3)).has_value());
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }

    // Remove the value of `b`: the document stays, the value goes.
    ASSERT_TRUE(writer.value()->update_numeric_doc_value(id_term("b"), "n", std::nullopt).has_value());
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    EXPECT_EQ(sorted_values(numeric_values(reader.value())), (std::vector<int64_t>{1, 3}));

    // A second update builds on the previous generation.
    ASSERT_TRUE(writer.value()->update_numeric_doc_value(id_term("c"), "n", 30).has_value());
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }
    EXPECT_EQ(writer.value()->segment_infos().segments[0].doc_values_gen, 2);

    auto reopened = DirectoryReader::open(directory);
    ASSERT_TRUE(reopened.has_value());
    EXPECT_EQ(sorted_values(numeric_values(reopened.value())), (std::vector<int64_t>{1, 30}));
}

TEST(DocValuesUpdatesTest, DoesNotTouchDocumentsIndexedAfterTheUpdate) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(directory, IndexWriterConfig(analyzer));
    ASSERT_TRUE(writer.has_value());

    ASSERT_TRUE(writer.value()->add_document(numeric_doc("a", 1)).has_value());
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }

    // Buffered while the index holds one document ...
    ASSERT_TRUE(writer.value()->update_numeric_doc_value(id_term("a"), "n", 100).has_value());
    // ... then a second document with the same id arrives.
    ASSERT_TRUE(writer.value()->add_document(numeric_doc("a", 2)).has_value());
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    const auto values = numeric_values(reader.value());
    ASSERT_EQ(values.size(), 2U);
    EXPECT_EQ(values.at(0), 100);
    EXPECT_EQ(values.at(1), 2);
}

TEST(DocValuesUpdatesTest, ANoOpUpdateWritesNothing) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(directory, IndexWriterConfig(analyzer));
    ASSERT_TRUE(writer.has_value());

    ASSERT_TRUE(writer.value()->add_document(numeric_doc("a", 1)).has_value());
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }
    const SegmentCommitInfo &segment = writer.value()->segment_infos().segments[0];
    const std::set<std::string> before(segment.info.files.begin(), segment.info.files.end());

    // No document matches the term: nothing is written.
    ASSERT_TRUE(writer.value()->update_numeric_doc_value(id_term("missing"), "n", 7).has_value());
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }

    EXPECT_EQ(segment.doc_values_gen, -1);
    EXPECT_EQ(segment.field_infos_gen, -1);
    EXPECT_EQ(std::set<std::string>(segment.info.files.begin(), segment.info.files.end()), before);
}

TEST(DocValuesUpdatesTest, SelectsTheDocumentsOfEveryMatchingSegment) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(directory, IndexWriterConfig(analyzer));
    ASSERT_TRUE(writer.has_value());

    ASSERT_TRUE(writer.value()->add_document(numeric_doc("a", 1)).has_value());
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }
    ASSERT_TRUE(writer.value()->add_document(numeric_doc("a", 2)).has_value());
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }
    ASSERT_EQ(writer.value()->segment_infos().segments.size(), 2U);

    ASSERT_TRUE(writer.value()->update_numeric_doc_value(id_term("a"), "n", 50).has_value());
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }

    // Both segments got their own generation.
    EXPECT_EQ(writer.value()->segment_infos().segments[0].doc_values_gen, 1);
    EXPECT_EQ(writer.value()->segment_infos().segments[1].doc_values_gen, 1);

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    EXPECT_EQ(sorted_values(numeric_values(reader.value())), (std::vector<int64_t>{50, 50}));
}

TEST(DocValuesUpdatesTest, TheNearRealTimeReaderSeesTheUpdate) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(directory, IndexWriterConfig(analyzer));
    ASSERT_TRUE(writer.has_value());

    ASSERT_TRUE(writer.value()->add_document(numeric_doc("a", 1)).has_value());
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }
    ASSERT_TRUE(writer.value()->update_numeric_doc_value(id_term("a"), "n", 42).has_value());

    auto reader = writer.value()->get_reader();
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_EQ(numeric_values(reader.value()).at(0), 42);

    // A second near-real-time read does not serve a stale generation.
    auto again = writer.value()->get_reader();
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(numeric_values(again.value()).at(0), 42);
}

TEST(DocValuesUpdatesTest, RejectsAFieldThatCannotBeUpdatedInPlace) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(directory, IndexWriterConfig(analyzer));
    ASSERT_TRUE(writer.has_value());

    Document doc = numeric_doc("a", 1);
    doc.add(std::make_unique<Field>("text", FieldValue{std::string("hello world")}, text_type()));
    ASSERT_TRUE(writer.value()->add_document(doc).has_value());
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }

    // Postings have no doc values at all.
    ASSERT_TRUE(writer.value()->update_numeric_doc_value(id_term("a"), "text", 5).has_value());
    auto committed = writer.value()->commit();
    ASSERT_FALSE(committed.has_value());
    EXPECT_NE(committed.error().to_string().find("in place"), std::string::npos);
}

TEST(DocValuesUpdatesTest, AMergeKeepsTheUpdatedValues) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(directory, IndexWriterConfig(analyzer));
    ASSERT_TRUE(writer.has_value());

    ASSERT_TRUE(writer.value()->add_document(numeric_doc("a", 1)).has_value());
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }
    ASSERT_TRUE(writer.value()->add_document(numeric_doc("b", 2)).has_value());
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }
    ASSERT_TRUE(writer.value()->update_numeric_doc_value(id_term("b"), "n", 200).has_value());
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }

    ASSERT_TRUE(writer.value()->force_merge(1).has_value());
    ASSERT_EQ(writer.value()->segment_infos().segments.size(), 1U);
    // The merged segment starts over with no generation.
    EXPECT_EQ(writer.value()->segment_infos().segments[0].doc_values_gen, -1);

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    EXPECT_EQ(sorted_values(numeric_values(reader.value())), (std::vector<int64_t>{1, 200}));
}

TEST(FieldUpdatesBufferTest, BuffersAndResolvesUpdates) {
    FieldUpdatesBuffer buffer;
    EXPECT_FALSE(buffer.any());

    buffer.add_numeric(id_term("a"), "n", 10, 5);
    buffer.add_binary(id_term("a"), "b", 10, BytesRef::from_string("x"));
    EXPECT_TRUE(buffer.any());
    EXPECT_EQ(buffer.size(), 2U);

    const auto fields = buffer.fields();
    ASSERT_EQ(fields.size(), 2U);
    EXPECT_TRUE(fields.contains("n"));
    EXPECT_TRUE(fields.contains("b"));
    EXPECT_EQ(buffer.type_of("n"), DocValuesType::Numeric);
    EXPECT_EQ(buffer.type_of("b"), DocValuesType::Binary);
    EXPECT_EQ(buffer.type_of("missing"), DocValuesType::None);
    EXPECT_GT(buffer.ram_bytes_used(), 0);

    auto find_docs = [](const Term &term) -> Result<std::vector<int32_t>> {
        if (term.text() == "a") {
            return std::vector<int32_t>{1, 2};
        }
        return std::vector<int32_t>{};
    };
    auto updates = resolve_field_updates(buffer, "n", find_docs, /*doc_base=*/0);
    ASSERT_TRUE(updates.has_value()) << (updates ? "" : updates.error().to_string());
    EXPECT_EQ(updates.value()->size(), 2U);
    EXPECT_EQ(updates.value()->numeric_value(1), 5);
    EXPECT_EQ(updates.value()->numeric_value(2), 5);
    EXPECT_FALSE(updates.value()->numeric_value(3).has_value());
    EXPECT_TRUE(updates.value()->is_updated(2));
    EXPECT_FALSE(updates.value()->is_updated(3));

    // A document indexed after the update was buffered is left alone.
    buffer.clear();
    buffer.add_numeric(id_term("a"), "n", /*doc_id_upto=*/2, 9);
    auto later = resolve_field_updates(buffer, "n", find_docs, /*doc_base=*/0);
    ASSERT_TRUE(later.has_value());
    EXPECT_EQ(later.value()->size(), 1U);
    EXPECT_EQ(later.value()->numeric_value(1), 9);
    EXPECT_FALSE(later.value()->is_updated(2));
}

TEST(NumericDocValuesFieldUpdatesTest, KeepsTheLastUpdateOfADocument) {
    NumericDocValuesFieldUpdates updates("n", DocValuesType::Numeric);
    ASSERT_TRUE(updates.add_numeric(2, 20).has_value());
    ASSERT_TRUE(updates.add_numeric(0, 1).has_value());
    ASSERT_TRUE(updates.add_numeric(2, 21).has_value());
    EXPECT_EQ(updates.iterator(), nullptr);

    updates.finish();
    ASSERT_EQ(updates.size(), 2U);
    EXPECT_EQ(updates.docs(), (std::vector<int32_t>{0, 2}));
    EXPECT_EQ(updates.numeric_value(2), 21);
    EXPECT_FALSE(updates.numeric_value(1).has_value());

    auto iterator = updates.iterator();
    ASSERT_NE(iterator, nullptr);
    ASSERT_TRUE(iterator->next());
    EXPECT_EQ(iterator->doc(), 0);
    EXPECT_EQ(iterator->long_value().value(), 1);
    ASSERT_TRUE(iterator->next());
    EXPECT_EQ(iterator->doc(), 2);
    EXPECT_EQ(iterator->long_value().value(), 21);
    EXPECT_FALSE(iterator->next());
    EXPECT_EQ(iterator->doc(), NO_MORE_DOCS);
    EXPECT_FALSE(iterator->binary_value().has_value());

    // A numeric batch refuses a binary value, and the reverse.
    EXPECT_FALSE(updates.add_binary(0, BytesRef::from_string("x")).has_value());
    BinaryDocValuesFieldUpdates binary("b", DocValuesType::Binary);
    EXPECT_FALSE(binary.add_numeric(0, 1).has_value());
}

TEST(BinaryDocValuesFieldUpdatesTest, IteratesTheDocumentsThatKeepAValue) {
    BinaryDocValuesFieldUpdates updates("b", DocValuesType::Binary);
    ASSERT_TRUE(updates.add_binary(2, BytesRef::from_string("two")).has_value());
    ASSERT_TRUE(updates.add_binary(0, std::nullopt).has_value());
    ASSERT_TRUE(updates.add_binary(1, BytesRef::from_string("one")).has_value());
    updates.finish();

    EXPECT_EQ(updates.size(), 3U);
    EXPECT_TRUE(updates.is_updated(0));
    EXPECT_FALSE(updates.binary_value(0).has_value());
    ASSERT_TRUE(updates.binary_value(1).has_value());
    EXPECT_EQ(updates.binary_value(1).value().utf8_to_string().value(), "one");

    // The removal is skipped: only the documents that keep a value come out.
    auto iterator = updates.iterator();
    ASSERT_NE(iterator, nullptr);
    ASSERT_TRUE(iterator->next());
    EXPECT_EQ(iterator->doc(), 1);
    ASSERT_TRUE(iterator->next());
    EXPECT_EQ(iterator->doc(), 2);
    EXPECT_FALSE(iterator->next());
    EXPECT_GT(updates.ram_bytes_used(), 0);
}

TEST(DocValuesUpdatesTest, AppliesABatchOfUpdates) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_merge_policy(NoMergePolicy::create());
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value());

    ASSERT_TRUE(writer.value()->add_document(numeric_doc("a", 1)).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    const std::vector<DocValuesUpdate> batch{
            DocValuesUpdate::create(DocValuesType::Numeric, id_term("a"), "n", 10,
                                    DocValuesUpdateValue{NumericDocValuesUpdate::of(7)})};
    auto applied = writer.value()->update_doc_values(batch);
    ASSERT_TRUE(applied.has_value()) << (applied ? "" : applied.error().to_string());
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    EXPECT_EQ(numeric_values(reader.value()).at(0), 7);
}

TEST(DocValuesUpdatesTest, UpdatesOneDocumentOfAReader) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_merge_policy(NoMergePolicy::create());
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value());

    ASSERT_TRUE(writer.value()->add_document(numeric_doc("a", 1)).has_value());
    ASSERT_TRUE(writer.value()->add_document(numeric_doc("b", 2)).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());

    const std::vector<DocValuesUpdate> batch{
            DocValuesUpdate::create(DocValuesType::Numeric, id_term("ignored"), "n", 10,
                                    DocValuesUpdateValue{NumericDocValuesUpdate::of(42)})};
    auto updated = writer.value()->try_update_doc_value(*reader.value(), 1, batch);
    ASSERT_TRUE(updated.has_value()) << (updated ? "" : updated.error().to_string());
    EXPECT_TRUE(updated.value());
    {
        auto committed = writer.value()->commit();
        ASSERT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
    }

    auto reopened = DirectoryReader::open(directory);
    ASSERT_TRUE(reopened.has_value());
    const auto values = numeric_values(reopened.value());
    ASSERT_EQ(values.size(), 2U);
    EXPECT_EQ(values.at(0), 1);
    EXPECT_EQ(values.at(1), 42);

    // A document the reader does not have is refused.
    auto missing = writer.value()->try_update_doc_value(*reader.value(), 100, batch);
    ASSERT_TRUE(missing.has_value());
    EXPECT_FALSE(missing.value());
}
