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

// `MMapDirectory`: the file system directory that maps every file it reads, under
// the name the reference implementation gives it.

module;

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.mmap_directory;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kBody = "body";

FieldType body_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

void build_index(Directory &directory, int32_t docs) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());
    for (int32_t i = 0; i < docs; ++i) {
        Document document;
        document.add(std::make_unique<Field>(std::string(kBody), FieldValue{std::string("alpha")}, body_type()));
        ASSERT_TRUE(writer.value()->add_document(std::move(document)).has_value());
    }
    ASSERT_TRUE(writer.value()->close().has_value());
}

} // namespace

TEST(MMapDirectoryTest, WritesAndReadsAnIndexThroughTheMapping) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "clucene_mmap_directory";
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);

    auto directory = MMapDirectory::open(path);
    ASSERT_TRUE(directory.has_value()) << (directory.has_value() ? "" : directory.error().to_string());
    EXPECT_TRUE(directory.value()->use_mmap());
    EXPECT_EQ(directory.value()->to_string(), "MMapDirectory(" + path.string() + ")");

    build_index(*directory.value(), 40);

    // The files are all there and every read goes through a mapping.
    auto files = directory.value()->list_all();
    ASSERT_TRUE(files.has_value()) << (files.has_value() ? "" : files.error().to_string());
    EXPECT_FALSE(files.value().empty());
    for (const std::string &name : files.value()) {
        auto length = directory.value()->file_length(name);
        ASSERT_TRUE(length.has_value()) << name;
        if (name.ends_with(".lck")) {
            // The write lock the writer held while it ran: empty by design.
            EXPECT_EQ(length.value(), 0U) << name;
            continue;
        }
        EXPECT_GT(length.value(), 0U) << name;
        auto input = directory.value()->open_input(name, IOContext());
        ASSERT_TRUE(input.has_value()) << (input.has_value() ? name : input.error().to_string());
        // A mapped input answers a positional read of the whole file.
        std::vector<uint8_t> bytes(length.value(), 0);
        ASSERT_TRUE(input.value()->read_bytes(bytes).has_value()) << name;
        EXPECT_EQ(input.value()->length().value(), length.value()) << name;
    }

    auto reader = DirectoryReader::open(*directory.value());
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    IndexSearcher searcher(reader.value());
    auto counted = searcher.count(MatchAllDocsQuery());
    ASSERT_TRUE(counted.has_value()) << counted.error().to_string();
    EXPECT_EQ(counted.value(), 40);

    CheckIndex check(*directory.value());
    auto checked = check.check();
    ASSERT_TRUE(checked.has_value()) << (checked.has_value() ? "" : checked.error().to_string());
    EXPECT_TRUE(checked.value().clean()) << checked.value().to_string();

    std::filesystem::remove_all(path, ignored);
}

TEST(MMapDirectoryTest, AFreshDirectoryIsEmpty) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "clucene_mmap_directory_empty";
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);

    auto directory = MMapDirectory::open(path);
    ASSERT_TRUE(directory.has_value()) << (directory.has_value() ? "" : directory.error().to_string());
    auto files = directory.value()->list_all();
    ASSERT_TRUE(files.has_value());
    EXPECT_TRUE(files.value().empty());

    // Reading a file that is not there is an error, not a crash.
    auto missing = directory.value()->open_input("segments_1", IOContext());
    EXPECT_FALSE(missing.has_value());

    std::filesystem::remove_all(path, ignored);
}
