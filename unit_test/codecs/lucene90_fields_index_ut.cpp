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
#include <utility>
#include <vector>

module lucene_core:ut.lucene90_fields_index;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view TEST_CODEC = "Lucene90TestFields";
constexpr std::string_view TEST_EXTENSION = "tsx";
constexpr std::string_view META_NAME = "_0.dum";

struct Fixture {
    ByteBuffersDirectory directory;
    std::vector<uint8_t> id{std::vector<uint8_t>(ID_LENGTH, 0x11)};
    /// The chunk descriptions: documents per chunk, then the pointer the chunk
    /// starts at; the pointers are contiguous, as in a real segmented file.
    std::vector<std::pair<int32_t, size_t>> chunks{{10, 0}, {10, 1000}, {5, 2100}, {15, 2500}};
    size_t max_pointer{4000};
    int32_t num_docs{40};
};

/// Writes the index of a fixture and returns the metadata file name to read.
void write_index(Fixture &fixture) {
    auto meta_out = fixture.directory.create_output(META_NAME, io_context_default());
    ASSERT_TRUE(meta_out.has_value()) << (meta_out ? "" : meta_out.error().to_string());

    auto writer = Lucene90FieldsIndexWriter::create(fixture.directory, "_0", "", TEST_EXTENSION, TEST_CODEC,
                                                    fixture.id, /*block_shift=*/3, io_context_default());
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    for (const auto &[num_docs, pointer] : fixture.chunks) {
        ASSERT_TRUE(writer.value()->write_index(num_docs, pointer).has_value());
    }
    auto finished = writer.value()->finish(fixture.num_docs, fixture.max_pointer, *meta_out.value());
    ASSERT_TRUE(finished.has_value()) << (finished ? "" : finished.error().to_string());
    ASSERT_TRUE(meta_out.value()->close().has_value());
}

std::unique_ptr<Lucene90FieldsIndexReader> open_index(Fixture &fixture) {
    auto meta_in = fixture.directory.open_input(META_NAME, io_context_default());
    EXPECT_TRUE(meta_in.has_value());
    if (!meta_in.has_value()) {
        return nullptr;
    }
    auto reader = Lucene90FieldsIndexReader::create(fixture.directory, "_0", "", TEST_EXTENSION, TEST_CODEC,
                                                    fixture.id, *meta_in.value(), io_context_default());
    EXPECT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    return reader.has_value() ? std::move(reader.value()) : nullptr;
}

} // namespace

TEST(Lucene90FieldsIndexTest, MapsDocumentsToChunks) {
    Fixture fixture;
    write_index(fixture);

    auto reader = open_index(fixture);
    ASSERT_TRUE(reader != nullptr);

    // Doc ids at both ends of every chunk and at every boundary.
    const std::vector<std::pair<int32_t, int64_t>> expected{{0, 0}, {9, 0}, {10, 1}, {19, 1},
                                                            {20, 2}, {24, 2}, {25, 3}, {39, 3}};
    for (const auto &[doc, chunk] : expected) {
        auto block = reader->get_block_id(doc);
        ASSERT_TRUE(block.has_value()) << (block ? "" : block.error().to_string());
        EXPECT_EQ(block.value(), chunk) << "doc " << doc;
    }

    // Out-of-range documents are rejected rather than mapped somewhere.
    EXPECT_FALSE(reader->get_block_id(fixture.num_docs).has_value());
    EXPECT_FALSE(reader->get_block_id(-1).has_value());
}

TEST(Lucene90FieldsIndexTest, ReportsChunkPointersAndLengths) {
    Fixture fixture;
    write_index(fixture);

    auto reader = open_index(fixture);
    ASSERT_TRUE(reader != nullptr);

    EXPECT_EQ(reader->get_max_pointer(), fixture.max_pointer);
    for (size_t chunk = 0; chunk < fixture.chunks.size(); ++chunk) {
        auto pointer = reader->get_block_start_pointer(static_cast<int64_t>(chunk));
        ASSERT_TRUE(pointer.has_value()) << (pointer ? "" : pointer.error().to_string());
        EXPECT_EQ(pointer.value(), fixture.chunks[chunk].second) << "chunk " << chunk;
    }

    // A chunk ends where the next one starts, and the last one ends at the
    // pointer the writer was finished with.
    const std::vector<size_t> lengths{1000, 1100, 400, 1500};
    for (size_t chunk = 0; chunk < lengths.size(); ++chunk) {
        auto length = reader->get_block_length(static_cast<int64_t>(chunk));
        ASSERT_TRUE(length.has_value()) << (length ? "" : length.error().to_string());
        EXPECT_EQ(length.value(), lengths[chunk]) << "chunk " << chunk;
    }

    // The convenience method adds the two lookups up.
    auto start = reader->get_start_pointer(15);
    ASSERT_TRUE(start.has_value()) << (start ? "" : start.error().to_string());
    EXPECT_EQ(start.value(), 1000);
}

TEST(Lucene90FieldsIndexTest, VerifiesTheIndexChecksum) {
    Fixture fixture;
    write_index(fixture);

    auto reader = open_index(fixture);
    ASSERT_TRUE(reader != nullptr);
    EXPECT_TRUE(reader->check_integrity().has_value());
}

TEST(Lucene90FieldsIndexTest, ClonesIntoAnIndependentReader) {
    Fixture fixture;
    write_index(fixture);

    auto reader = open_index(fixture);
    ASSERT_TRUE(reader != nullptr);
    auto copy = reader->clone();
    ASSERT_TRUE(copy.has_value()) << (copy ? "" : copy.error().to_string());

    // Closing the original leaves the copy usable, which is the point of cloning.
    ASSERT_TRUE(reader->close().has_value());
    auto block = copy.value()->get_block_id(30);
    ASSERT_TRUE(block.has_value()) << (block ? "" : block.error().to_string());
    EXPECT_EQ(block.value(), 3);
    EXPECT_TRUE(copy.value()->check_integrity().has_value());
}

TEST(Lucene90FieldsIndexTest, DeletesTheTemporaryFiles) {
    Fixture fixture;
    write_index(fixture);

    auto files = fixture.directory.list_all();
    ASSERT_TRUE(files.has_value());
    std::vector<std::string> expected{"_0.dum", "_0.tsx"};
    EXPECT_EQ(files.value(), expected);
}

TEST(Lucene90FieldsIndexTest, ClosesUnfinishedWriters) {
    Fixture fixture;
    auto writer = Lucene90FieldsIndexWriter::create(fixture.directory, "_0", "", TEST_EXTENSION, TEST_CODEC,
                                                    fixture.id, /*block_shift=*/3, io_context_default());
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer.value()->write_index(10, 0).has_value());

    // Dropping an unfinished writer must not leave its temporary files behind.
    EXPECT_TRUE(writer.value()->close().has_value());
    auto files = fixture.directory.list_all();
    ASSERT_TRUE(files.has_value());
    EXPECT_TRUE(files.value().empty()) << files.value().front();
}
