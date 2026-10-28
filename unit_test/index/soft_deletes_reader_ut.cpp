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
#include <filesystem>
#include <memory>
#include <span>
#include <string>

module lucene_core:ut.soft_deletes_reader;

import lucene_core;

using namespace lucene;

namespace {

constexpr int32_t DOC_COUNT = 300;
constexpr std::string_view SOFT_DELETE_FIELD = "deleted";

FieldType tag_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::Docs).has_value());
    return type;
}

/// `id % 3 == 0` documents are marked soft-deleted by writing `1` into the soft
/// delete field, which is what the reference implementation's writer does.
Document make_doc(int32_t id) {
    Document doc;
    doc.add(std::make_unique<Field>("id", FieldValue{std::to_string(id)}, tag_type()));
    doc.add(std::make_unique<Field>("tag", FieldValue{std::string("t") + std::to_string(id % 5)}, tag_type()));

    auto deleted = NumericDocValuesField::create(std::string(SOFT_DELETE_FIELD), id % 3 == 0 ? 1 : 0);
    EXPECT_TRUE(deleted.has_value());
    doc.add(std::make_unique<Field>(std::move(deleted.value())));
    return doc;
}

struct IndexFixture {
    std::filesystem::path path;

    explicit IndexFixture(std::string_view name) {
        path = std::filesystem::temp_directory_path() / std::string(name);
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    ~IndexFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    IndexFixture(const IndexFixture &) = delete;
    IndexFixture &operator=(const IndexFixture &) = delete;
};

/// Builds an index of DOC_COUNT documents and returns the open directory.
std::shared_ptr<Directory> build_index(const std::filesystem::path &path) {
    auto directory = FSDirectory::open(path);
    EXPECT_TRUE(directory.has_value());
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(*directory.value(), config);
    EXPECT_TRUE(writer.has_value());
    for (int32_t id = 0; id < DOC_COUNT; ++id) {
        EXPECT_TRUE(writer.value()->add_document(make_doc(id)).has_value());
    }
    EXPECT_TRUE(writer.value()->commit().has_value());
    EXPECT_TRUE(writer.value()->close().has_value());
    return std::move(directory.value());
}

} // namespace

TEST(SoftDeletesReaderTest, HidesSoftDeletedDocuments) {
    IndexFixture fixture("lucene_core_soft_deletes_ut");
    auto directory = build_index(fixture.path);

    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->num_docs().value(), DOC_COUNT);

    auto wrapped = wrap_soft_deletes_directory_reader(reader.value(), std::string(SOFT_DELETE_FIELD));
    ASSERT_TRUE(wrapped.has_value()) << (wrapped ? "" : wrapped.error().to_string());
    auto &filtered = *wrapped.value();

    // One third of the documents carries `1`: they are gone, but max_doc stays.
    const int32_t soft_deleted = (DOC_COUNT + 2) / 3;
    EXPECT_EQ(filtered.max_doc().value(), DOC_COUNT);
    EXPECT_EQ(filtered.num_docs().value(), DOC_COUNT - soft_deleted);
    EXPECT_EQ(filtered.num_deleted_docs().value(), soft_deleted);
    EXPECT_TRUE(filtered.has_deletions().value());

    // Searching only sees the surviving documents.
    IndexSearcher searcher(wrapped.value());
    auto all = searcher.search(MatchAllDocsQuery(), 10);
    ASSERT_TRUE(all.has_value()) << (all ? "" : all.error().to_string());
    EXPECT_EQ(all.value().total_hits.value, DOC_COUNT - soft_deleted);

    // A single soft-deleted document is invisible ...
    auto hidden = searcher.search(TermQuery(Term::from_text("id", "0")), 10);
    ASSERT_TRUE(hidden.has_value());
    EXPECT_EQ(hidden.value().total_hits.value, 0);
    // ... while a live one is still found.
    auto visible = searcher.search(TermQuery(Term::from_text("id", "1")), 10);
    ASSERT_TRUE(visible.has_value());
    EXPECT_EQ(visible.value().total_hits.value, 1);
}

TEST(SoftDeletesReaderTest, ReturnsTheReaderUnchangedWhenNothingIsSoftDeleted) {
    IndexFixture fixture("lucene_core_soft_deletes_none_ut");
    auto directory = build_index(fixture.path);
    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value());

    // The field does not exist at all.
    auto wrapped = wrap_soft_deletes_directory_reader(reader.value(), "no_such_field");
    ASSERT_TRUE(wrapped.has_value()) << (wrapped ? "" : wrapped.error().to_string());
    EXPECT_EQ(wrapped.value()->num_docs().value(), DOC_COUNT);

    // The field exists but no document is marked.
    auto none = wrap_soft_deletes_directory_reader(reader.value(), "always_zero");
    ASSERT_TRUE(none.has_value());
    EXPECT_EQ(none.value()->num_docs().value(), DOC_COUNT);
}

TEST(SoftDeletesReaderTest, CombinesWithHardDeletes) {
    IndexFixture fixture("lucene_core_soft_deletes_hard_ut");
    {
        auto directory = FSDirectory::open(fixture.path);
        ASSERT_TRUE(directory.has_value());
        WhitespaceAnalyzer analyzer;
        IndexWriterConfig config(analyzer);
        auto writer = IndexWriter::open(*directory.value(), config);
        ASSERT_TRUE(writer.has_value());
        for (int32_t id = 0; id < 30; ++id) {
            ASSERT_TRUE(writer.value()->add_document(make_doc(id)).has_value());
        }
        // Hard delete every document whose id ends in 1.
        ASSERT_TRUE(writer.value()->delete_documents(Term::from_text("id", "11")).has_value());
        ASSERT_TRUE(writer.value()->commit().has_value());
        ASSERT_TRUE(writer.value()->close().has_value());
    }

    auto directory = FSDirectory::open(fixture.path);
    ASSERT_TRUE(directory.has_value());
    auto reader = DirectoryReader::open(*directory.value());
    ASSERT_TRUE(reader.has_value());
    const int32_t hard_deleted = 1;
    const int32_t soft_deleted = 10; // 0, 3, 6, ... 27
    ASSERT_EQ(reader.value()->num_docs().value(), 30 - hard_deleted);

    auto wrapped = wrap_soft_deletes_directory_reader(reader.value(), std::string(SOFT_DELETE_FIELD));
    ASSERT_TRUE(wrapped.has_value()) << (wrapped ? "" : wrapped.error().to_string());
    // The hard delete is preserved: it is not counted twice.
    EXPECT_EQ(wrapped.value()->num_docs().value(), 30 - hard_deleted - soft_deleted);
}

TEST(FilterCodecReaderBitsTest, SingleAndMixed) {
    auto first = std::make_shared<FixedBitSet>(8);
    first->set(0);
    first->set(2);
    first->set(4);

    FilterCodecReaderBits single(first);
    EXPECT_EQ(single.length(), 8U);
    EXPECT_TRUE(single.get(0).value());
    EXPECT_FALSE(single.get(1).value());
    EXPECT_NE(single.to_string().find("FilterCodecReaderBits"), std::string::npos);

    auto second = std::make_shared<FixedBitSet>(8);
    second->set(2);
    second->set(6);

    auto mixed = FilterCodecReaderBits::mixed(first, second);
    EXPECT_EQ(mixed->length(), 8U);
    EXPECT_FALSE(mixed->get(0).value()); // only in the hard set
    EXPECT_TRUE(mixed->get(2).value());  // in both
    EXPECT_FALSE(mixed->get(4).value()); // only in the hard set
    EXPECT_FALSE(mixed->get(6).value()); // only in the wrapped set
    EXPECT_NE(mixed->to_string().find("mixed"), std::string::npos);
}
