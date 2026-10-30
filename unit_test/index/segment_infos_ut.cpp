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
#include <map>
#include <set>
#include <string>
#include <vector>

module lucene_core:ut.segment_infos;

import lucene_core;

using namespace lucene;

namespace {

std::vector<uint8_t> id_of(uint8_t seed) {
    std::vector<uint8_t> id(ID_LENGTH, 0);
    for (size_t i = 0; i < id.size(); ++i) {
        id[i] = static_cast<uint8_t>(seed + i);
    }
    return id;
}

/// A segment whose `.si` file exists in `dir`, ready to be referenced.
SegmentCommitInfo committed_segment(Directory &dir, std::string name, uint8_t seed, int32_t max_doc) {
    SegmentInfo info;
    info.name = std::move(name);
    info.id = id_of(seed);
    info.version = Version{10, 1, 0};
    info.min_version = Version{10, 0, 0};
    info.max_doc = max_doc;
    info.diagnostics.emplace("source", "flush");
    info.codec_name = std::string(Lucene99Codec::CODEC_NAME);

    const auto &format = lucene99_codec().segment_info_format();
    EXPECT_TRUE(format.write(dir, info, io_context_default()).has_value());

    SegmentCommitInfo segment;
    segment.info = std::move(info);
    return segment;
}

} // namespace

TEST(SegmentInfosTest, RoundTripsEveryCommitField) {
    ByteBuffersDirectory dir;

    SegmentInfos infos;
    infos.version = 3;
    infos.counter = 7;
    infos.generation = 1;
    infos.id = id_of(1);
    infos.user_data.emplace("author", "clucene");
    infos.user_data.emplace("build", "test");

    auto first = committed_segment(dir, "_0", 10, 42);
    first.del_gen = 2;
    first.del_count = 3;
    first.soft_del_count = 1;
    first.field_infos_gen = 4;
    first.doc_values_gen = 5;
    first.id = id_of(110);
    first.field_infos_files.insert("_0_1.fnm");
    first.doc_values_updates_files.emplace(6, std::set<std::string>{"_0_6.dvd", "_0_6.dvm"});
    ASSERT_TRUE(infos.add(std::move(first)).has_value());

    // A segment written by an older release: the commit records the minimum of
    // the segments' creation versions as `minSegmentLuceneVersion`.
    auto second = committed_segment(dir, "_1", 20, 8);
    second.info.version = Version{10, 0, 0};
    ASSERT_TRUE(infos.add(std::move(second)).has_value());

    ASSERT_TRUE(infos.write_to(dir, io_context_default()).has_value());
    // Two `.si` files plus the commit file.
    EXPECT_EQ(dir.file_count(), 3u);
    EXPECT_EQ(infos.generation_file_name(), "segments_1");

    auto read = SegmentInfos::read(dir, 1);
    ASSERT_TRUE(read.has_value()) << (read ? "" : read.error().to_string());
    EXPECT_EQ(read->version, 3);
    EXPECT_EQ(read->counter, 7);
    EXPECT_EQ(read->generation, 1);
    EXPECT_EQ(read->last_generation, 1);
    EXPECT_EQ(read->id, infos.id);
    EXPECT_EQ(read->index_created_version_major, 10);
    EXPECT_EQ(read->lucene_version, (Version{10, 1, 0}));
    ASSERT_TRUE(read->min_segment_lucene_version.has_value());
    EXPECT_EQ(read->min_segment_lucene_version.value(), (Version{10, 0, 0}));
    EXPECT_EQ(read->user_data, infos.user_data);
    ASSERT_EQ(read->size(), 2u);

    const SegmentCommitInfo &actual = read->segments[0];
    EXPECT_EQ(actual.info.name, "_0");
    EXPECT_EQ(actual.info.id, id_of(10));
    EXPECT_EQ(actual.info.codec_name, "Lucene99");
    EXPECT_EQ(actual.info.max_doc, 42);
    EXPECT_EQ(actual.del_gen, 2);
    EXPECT_EQ(actual.del_count, 3);
    EXPECT_EQ(actual.soft_del_count, 1);
    EXPECT_EQ(actual.field_infos_gen, 4);
    EXPECT_EQ(actual.doc_values_gen, 5);
    ASSERT_TRUE(actual.id.has_value());
    EXPECT_EQ(actual.id.value(), id_of(110));
    EXPECT_EQ(actual.field_infos_files, (std::set<std::string>{"_0_1.fnm"}));
    ASSERT_EQ(actual.doc_values_updates_files.size(), 1u);
    EXPECT_EQ(actual.doc_values_updates_files.at(6).size(), 2u);
    // 42 documents, 3 hard and 1 soft deletion.
    EXPECT_EQ(actual.doc_count(), 38);
    EXPECT_EQ(actual.total_del_count(), 4);

    EXPECT_EQ(read->segments[1].info.name, "_1");
    EXPECT_FALSE(read->segments[1].id.has_value());
    EXPECT_EQ(read->total_max_doc(), 50);
}

TEST(SegmentInfosTest, RoundTripsAnEmptyIndex) {
    ByteBuffersDirectory dir;
    SegmentInfos infos;
    infos.version = 1;
    infos.counter = 0;
    infos.generation = 1;
    infos.id = id_of(3);

    ASSERT_TRUE(infos.write_to(dir, io_context_default()).has_value());
    auto read = SegmentInfos::read(dir, 1);
    ASSERT_TRUE(read.has_value()) << (read ? "" : read.error().to_string());
    EXPECT_TRUE(read->empty());
    EXPECT_FALSE(read->min_segment_lucene_version.has_value());
    EXPECT_EQ(read->total_max_doc(), 0);
}

TEST(SegmentInfosTest, ReadsTheNumericallyNewestGeneration) {
    ByteBuffersDirectory dir;
    SegmentInfos infos;
    infos.id = id_of(4);

    // 9 and 10 sort the wrong way as text ("10" < "9"), which is why the
    // generation has to be parsed, not compared byte by byte.
    infos.generation = 9;
    infos.version = 1;
    ASSERT_TRUE(infos.write_to(dir, io_context_default()).has_value());
    infos.generation = 10;
    infos.version = 2;
    ASSERT_TRUE(infos.write_to(dir, io_context_default()).has_value());

    auto generations = SegmentInfos::list_generations(dir);
    ASSERT_TRUE(generations.has_value());
    EXPECT_EQ(generations.value(), (std::vector<int64_t>{9, 10}));

    auto latest = SegmentInfos::read_latest(dir);
    ASSERT_TRUE(latest.has_value()) << (latest ? "" : latest.error().to_string());
    EXPECT_EQ(latest->generation, 10);
    EXPECT_EQ(latest->version, 2);
}

TEST(SegmentInfosTest, RejectsInvalidCommitState) {
    ByteBuffersDirectory dir;

    SegmentInfos infos;
    infos.id = id_of(5);
    auto segment = committed_segment(dir, "_0", 30, 4);
    segment.del_count = 5;  // more deletions than documents
    EXPECT_FALSE(infos.add(std::move(segment)).has_value());
    EXPECT_TRUE(infos.empty());

    auto good = committed_segment(dir, "_1", 40, 4);
    good.soft_del_count = 3;
    good.del_count = 3;
    EXPECT_FALSE(infos.add(std::move(good)).has_value());

    // A segment without a minVersion cannot be committed by an index created
    // with Lucene 7 or later.
    auto without_min_version = committed_segment(dir, "_2", 50, 1);
    without_min_version.info.min_version.reset();
    ASSERT_TRUE(infos.add(std::move(without_min_version)).has_value());
    infos.generation = 1;
    EXPECT_FALSE(infos.write_to(dir, io_context_default()).has_value());
}

TEST(SegmentInfosTest, ReportsAMissingCommitFile) {
    ByteBuffersDirectory dir;
    auto latest = SegmentInfos::read_latest(dir);
    ASSERT_FALSE(latest.has_value());
    EXPECT_EQ(latest.error().kind(), ErrorKind::kIndexNotFound);

    auto missing = SegmentInfos::read(dir, 3);
    ASSERT_FALSE(missing.has_value());
}

TEST(SegmentInfosTest, RejectsACorruptSegmentReference) {
    ByteBuffersDirectory dir;
    SegmentInfos infos;
    infos.id = id_of(6);
    infos.generation = 1;
    ASSERT_TRUE(infos.add(committed_segment(dir, "_0", 60, 2)).has_value());
    ASSERT_TRUE(infos.write_to(dir, io_context_default()).has_value());

    // Without the `.si` file the commit cannot be opened: the segment metadata is
    // the authority for maxDoc and the file list.
    ASSERT_TRUE(dir.delete_file("_0.si").has_value());
    auto read = SegmentInfos::read(dir, 1);
    ASSERT_FALSE(read.has_value());
}

TEST(IndexFileNamesTest, BuildsAndParsesGenerations) {
    EXPECT_EQ(IndexFileNames::file_name_from_generation(IndexFileNames::SEGMENTS, "", 0), "segments_0");
    EXPECT_EQ(IndexFileNames::file_name_from_generation(IndexFileNames::SEGMENTS, "", 35), "segments_z");
    EXPECT_EQ(IndexFileNames::file_name_from_generation(IndexFileNames::SEGMENTS, "", 36), "segments_10");
    EXPECT_EQ(IndexFileNames::file_name_from_generation(IndexFileNames::PENDING_SEGMENTS, "", 7), "pending_segments");

    // base 36: "1f" == 1 * 36 + 15.
    EXPECT_EQ(IndexFileNames::generation_from_segment_file_name("segments_1f", IndexFileNames::SEGMENTS).value(), 51);
    EXPECT_FALSE(IndexFileNames::generation_from_segment_file_name("segments.gen", IndexFileNames::SEGMENTS)
                     .has_value());
    EXPECT_FALSE(IndexFileNames::generation_from_segment_file_name("other_1", IndexFileNames::SEGMENTS).has_value());

    EXPECT_EQ(IndexFileNames::base36_encode(0), "0");
    EXPECT_EQ(IndexFileNames::base36_encode(1295), "zz");
    EXPECT_EQ(IndexFileNames::base36_decode("zz").value(), 1295);
    EXPECT_EQ(IndexFileNames::base36_decode("ZZ").value(), 1295);
    EXPECT_FALSE(IndexFileNames::base36_decode("").has_value());
    EXPECT_FALSE(IndexFileNames::base36_decode("1!").has_value());
    // Round trip through the largest generation a commit file can have.
    const int64_t large = std::numeric_limits<int64_t>::max();
    EXPECT_EQ(IndexFileNames::base36_decode(IndexFileNames::base36_encode(large)).value(), large);

    EXPECT_EQ(IndexFileNames::strip_extension("_0.fnm"), "_0");
    EXPECT_EQ(IndexFileNames::strip_extension("segments_1"), "segments_1");
    EXPECT_EQ(IndexFileNames::strip_segment_name("segments_1", IndexFileNames::SEGMENTS), "1");

    // The one-argument overload cuts at the second `_` (that is where a `.del`
    // file keeps its generation), otherwise at the first `.`.
    EXPECT_EQ(IndexFileNames::strip_segment_name("_0.fnm"), ".fnm");
    EXPECT_EQ(IndexFileNames::strip_segment_name("_0_Lucene90_0.tim"), "_Lucene90_0.tim");
    EXPECT_EQ(IndexFileNames::strip_segment_name("_0_1.del"), "_1.del");
    EXPECT_EQ(IndexFileNames::strip_segment_name("_12.si"), ".si");
    EXPECT_EQ(IndexFileNames::strip_segment_name("segments_1"), "_1");
    EXPECT_EQ(IndexFileNames::strip_segment_name("noextension"), "noextension");
    // Re-prefixing the segment name rebuilds the original name, which is what the
    // compound file reader does when it lists its entries.
    EXPECT_EQ(std::string("_0") + IndexFileNames::strip_segment_name("_0.fnm"), "_0.fnm");
}
