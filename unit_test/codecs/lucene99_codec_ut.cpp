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
#include <span>
#include <string>
#include <vector>

module lucene_core:ut.lucene99_codec;

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

SegmentInfo make_segment_info() {
    SegmentInfo info;
    info.name = "_0";
    info.id = id_of(1);
    info.version = Version{10, 1, 0};
    info.min_version = Version{10, 0, 0};
    info.max_doc = 42;
    info.use_compound_file = false;
    info.has_blocks = false;
    info.diagnostics.emplace("source", "flush");
    info.diagnostics.emplace("lucene.version", "10.1.0");
    info.attributes.emplace("Lucene90.FieldInfosFormat.FieldCount", "3");
    return info;
}

FieldInfo numeric_field(int32_t number, std::string name) {
    FieldInfo field;
    field.number = number;
    field.name = std::move(name);
    field.index_options = IndexOptions::DocsAndFreqsAndPositions;
    field.doc_values_type = DocValuesType::Numeric;
    field.doc_values_gen = 7;
    field.point_dimension_count = 1;
    field.point_index_dimension_count = 1;
    field.point_num_bytes = 8;
    field.attributes.emplace("k", "v");
    return field;
}

} // namespace

TEST(Lucene99CodecTest, ExposesTheMetadataFormats) {
    const Codec &codec = lucene99_codec();
    EXPECT_EQ(codec.name(), "Lucene99");
    EXPECT_EQ(codec.segment_info_format().name(), "Lucene90SegmentInfo");
    EXPECT_EQ(Lucene99Codec::field_infos_codec_name(), "Lucene94FieldInfos");
    EXPECT_EQ(Lucene99SegmentInfoFormat::SI_EXTENSION, "si");
    EXPECT_EQ(Lucene94FieldInfosFormat::FNM_EXTENSION, "fnm");
}

TEST(Lucene99SegmentInfoFormatTest, RoundTripsEveryField) {
    ByteBuffersDirectory dir;
    SegmentInfo info = make_segment_info();

    const auto &format = lucene99_codec().segment_info_format();
    ASSERT_TRUE(format.write(dir, info, io_context_default()).has_value());
    EXPECT_EQ(dir.file_count(), 1u);
    // Writing the segment info registers the file with the segment.
    EXPECT_TRUE(info.files.contains("_0.si"));

    auto read = format.read(dir, "_0", info.id, io_context_default());
    ASSERT_TRUE(read.has_value()) << (read ? "" : read.error().to_string());
    const SegmentInfo &actual = read.value();
    EXPECT_EQ(actual.name, "_0");
    EXPECT_EQ(actual.id, info.id);
    ASSERT_TRUE(actual.version.has_value());
    EXPECT_EQ(actual.version.value(), (Version{10, 1, 0}));
    ASSERT_TRUE(actual.min_version.has_value());
    EXPECT_EQ(actual.min_version.value(), (Version{10, 0, 0}));
    EXPECT_EQ(actual.max_doc, 42);
    EXPECT_FALSE(actual.use_compound_file);
    EXPECT_FALSE(actual.has_blocks);
    EXPECT_EQ(actual.diagnostics, info.diagnostics);
    EXPECT_EQ(actual.files, info.files);
    EXPECT_EQ(actual.attributes, info.attributes);
    EXPECT_EQ(actual.sort_field_count, 0);
}

TEST(Lucene99SegmentInfoFormatTest, RoundTripsWithoutAMinimumVersion) {
    ByteBuffersDirectory dir;
    SegmentInfo info = make_segment_info();
    info.min_version.reset();
    info.use_compound_file = true;
    info.has_blocks = true;

    const auto &format = lucene99_codec().segment_info_format();
    ASSERT_TRUE(format.write(dir, info, io_context_default()).has_value());

    auto read = format.read(dir, "_0", info.id, io_context_default());
    ASSERT_TRUE(read.has_value()) << (read ? "" : read.error().to_string());
    EXPECT_FALSE(read->min_version.has_value());
    EXPECT_TRUE(read->use_compound_file);
    EXPECT_TRUE(read->has_blocks);
}

TEST(Lucene99SegmentInfoFormatTest, RejectsForeignFilesAndBadIds) {
    const auto &format = lucene99_codec().segment_info_format();

    {
        ByteBuffersDirectory dir;
        SegmentInfo info = make_segment_info();
        info.files.insert("_1.fnm");
        EXPECT_FALSE(format.write(dir, info, io_context_default()).has_value());
        EXPECT_EQ(dir.file_count(), 0u);
    }
    {
        ByteBuffersDirectory dir;
        SegmentInfo info = make_segment_info();
        info.id.resize(ID_LENGTH - 1);
        EXPECT_FALSE(format.write(dir, info, io_context_default()).has_value());
    }
    {
        ByteBuffersDirectory dir;
        SegmentInfo info = make_segment_info();
        info.version = std::nullopt;
        EXPECT_FALSE(format.write(dir, info, io_context_default()).has_value());
    }
}

TEST(Lucene99SegmentInfoFormatTest, RejectsAReadWithAForeignId) {
    ByteBuffersDirectory dir;
    SegmentInfo info = make_segment_info();
    const auto &format = lucene99_codec().segment_info_format();
    ASSERT_TRUE(format.write(dir, info, io_context_default()).has_value());

    // The id is part of the header, so a segment whose id changed (a copied file,
    // or a corrupted commit) is refused instead of silently accepted.
    const auto foreign_id = id_of(9);
    auto read = format.read(dir, "_0", foreign_id, io_context_default());
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error().kind(), ErrorKind::kCorruptIndex);
}

TEST(Lucene99SegmentInfoFormatTest, RejectsATruncatedFile) {
    ByteBuffersDirectory dir;
    SegmentInfo info = make_segment_info();
    const auto &format = lucene99_codec().segment_info_format();
    ASSERT_TRUE(format.write(dir, info, io_context_default()).has_value());

    // Replacing the file with a short one must fail the footer/header check
    // rather than read garbage.
    auto output = dir.create_output("_0.si");
    ASSERT_TRUE(output.has_value());
    ASSERT_TRUE(output.value()->write_byte(0x00).has_value());
    ASSERT_TRUE(output.value()->close().has_value());

    auto read = format.read(dir, "_0", info.id, io_context_default());
    ASSERT_FALSE(read.has_value());
    // A one byte file runs out of data before the header can match: an EOF, not a
    // codec mismatch, is what a truncated file looks like.
    EXPECT_TRUE(read.error().kind() == ErrorKind::kEof || read.error().kind() == ErrorKind::kCorruptIndex);
}

TEST(Lucene94FieldInfosFormatTest, RoundTripsEveryField) {
    ByteBuffersDirectory dir;
    std::vector<FieldInfo> fields;

    FieldInfo text;
    text.number = 0;
    text.name = "body";
    text.index_options = IndexOptions::DocsAndFreqsAndPositionsAndOffsets;
    text.store_term_vector = true;
    text.store_payloads = true;
    fields.push_back(text);

    fields.push_back(numeric_field(1, "price"));

    FieldInfo vector;
    vector.number = 2;
    vector.name = "embedding";
    vector.index_options = IndexOptions::None;
    vector.doc_values_type = DocValuesType::None;
    vector.vector_dimension = 128;
    vector.vector_encoding = VectorEncoding::Byte;
    vector.vector_similarity = VectorSimilarityFunction::Cosine;
    vector.omit_norms = true;
    fields.push_back(vector);

    auto infos = FieldInfos::of(std::move(fields));
    ASSERT_TRUE(infos.has_value()) << (infos ? "" : infos.error().to_string());

    const auto id = id_of(3);
    ASSERT_TRUE(Lucene94FieldInfosFormat::write(dir, "_0", "", id, infos.value(), io_context_default()).has_value());
    EXPECT_EQ(dir.file_count(), 1u);

    auto read = Lucene94FieldInfosFormat::read(dir, "_0", "", id, io_context_default());
    ASSERT_TRUE(read.has_value()) << (read ? "" : read.error().to_string());
    ASSERT_EQ(read->size(), 3u);

    const FieldInfo *body = read->by_name("body");
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(body->number, 0);
    EXPECT_EQ(body->index_options, IndexOptions::DocsAndFreqsAndPositionsAndOffsets);
    EXPECT_TRUE(body->has_offsets());
    EXPECT_TRUE(body->has_payloads());
    EXPECT_TRUE(body->has_term_vectors());
    EXPECT_TRUE(body->has_norms());

    const FieldInfo *price = read->by_number(1);
    ASSERT_NE(price, nullptr);
    EXPECT_EQ(price->name, "price");
    EXPECT_EQ(price->doc_values_type, DocValuesType::Numeric);
    EXPECT_EQ(price->doc_values_gen, 7);
    EXPECT_TRUE(price->has_point_values());
    EXPECT_EQ(price->attributes.at("k"), "v");

    const FieldInfo *embedding = read->by_name("embedding");
    ASSERT_NE(embedding, nullptr);
    EXPECT_EQ(embedding->vector_dimension, 128);
    EXPECT_EQ(embedding->vector_encoding, VectorEncoding::Byte);
    EXPECT_EQ(embedding->vector_similarity, VectorSimilarityFunction::Cosine);
    EXPECT_TRUE(embedding->has_vector_values());
    EXPECT_FALSE(embedding->is_indexed());
    EXPECT_FALSE(embedding->has_norms());

    EXPECT_EQ(read->indexed_fields(), (std::set<std::string>{"body", "price"}));
}

TEST(Lucene94FieldInfosFormatTest, WritesTheExpectedBytes) {
    // A round trip cannot catch a mistake the writer and the reader share, so pin
    // the layout: field number is stored explicitly, the skip index byte sits
    // between the doc values byte and the generation, and the generation is big
    // endian while the doc values type uses the on-disk numbering (SortedNumeric
    // is 5 there, but 4 in this port's enum).
    FieldInfo field;
    field.number = 0;
    field.name = "f";
    field.index_options = IndexOptions::DocsAndFreqs;
    field.doc_values_type = DocValuesType::SortedNumeric;

    auto infos = FieldInfos::of({field});
    ASSERT_TRUE(infos.has_value()) << (infos ? "" : infos.error().to_string());

    GrowableByteArrayDataOutput out;
    ASSERT_TRUE(Lucene94FieldInfosFormat::write_field_infos(out, infos.value()).has_value());
    const auto bytes = out.bytes();
    const std::vector<uint8_t> expected{
        0x01,                                  // field count
        0x01, 0x66,                            // "f"
        0x00,                                  // field number
        0x00,                                  // bits: no flags
        0x02,                                  // IndexOptions::DocsAndFreqs
        0x05,                                  // DocValuesType::SortedNumeric on disk
        0x00,                                  // DocValuesSkipIndexType::None
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // docValuesGen == -1
        0x00,                                  // attributes
        0x00,                                  // pointDimensionCount
        0x00,                                  // vectorDimension
        0x01,                                  // VectorEncoding::Float32
        0x00,                                  // VectorSimilarityFunction::Euclidean
    };
    EXPECT_EQ(std::vector<uint8_t>(bytes.begin(), bytes.end()), expected);
}

TEST(Lucene94FieldInfosFormatTest, RoundTripsAnEmptyFieldSet) {
    ByteBuffersDirectory dir;
    const auto id = id_of(4);
    ASSERT_TRUE(Lucene94FieldInfosFormat::write(dir, "_1", "", id, FieldInfos::none(), io_context_default())
                    .has_value());

    auto read = Lucene94FieldInfosFormat::read(dir, "_1", "", id, io_context_default());
    ASSERT_TRUE(read.has_value()) << (read ? "" : read.error().to_string());
    EXPECT_TRUE(read->empty());
}

TEST(Lucene94FieldInfosFormatTest, UsesTheSuffixInTheFileName) {
    ByteBuffersDirectory dir;
    const auto id = id_of(5);
    ASSERT_TRUE(Lucene94FieldInfosFormat::write(dir, "_2", "1", id, FieldInfos::none(), io_context_default())
                    .has_value());
    EXPECT_EQ(dir.file_count(), 1u);
    EXPECT_EQ(Lucene94FieldInfosFormat::file_name("_2", "1"), "_2_1.fnm");

    // Reading with the wrong suffix finds no file, rather than decoding another
    // generation's metadata.
    auto read = Lucene94FieldInfosFormat::read(dir, "_2", "2", id, io_context_default());
    EXPECT_FALSE(read.has_value());
}

TEST(FieldInfosTest, SortsByNumberAndRejectsBadNumbering) {
    auto unsorted = FieldInfos::of({numeric_field(1, "b"), numeric_field(0, "a")});
    ASSERT_TRUE(unsorted.has_value());
    EXPECT_EQ(unsorted->by_number(0)->name, "a");
    EXPECT_EQ(unsorted->by_number(1)->name, "b");
    EXPECT_EQ(unsorted->by_name("missing"), nullptr);
    EXPECT_EQ(unsorted->by_number(2), nullptr);

    EXPECT_FALSE(FieldInfos::of({numeric_field(0, "a"), numeric_field(2, "c")}).has_value());
    EXPECT_FALSE(FieldInfos::of({numeric_field(0, "a"), numeric_field(1, "a")}).has_value());

    FieldInfo bad_dimensions = numeric_field(0, "a");
    bad_dimensions.point_index_dimension_count = 2;
    EXPECT_FALSE(FieldInfos::of({bad_dimensions}).has_value());
}

TEST(SegmentInfoTest, BuildsFileNames) {
    EXPECT_EQ(segment_file_name("_0", "", "si"), "_0.si");
    EXPECT_EQ(segment_file_name("_0", "1", "fnm"), "_0_1.fnm");
    EXPECT_EQ(parse_segment_name("_3_Lucene99_0.dvm").value(), "_3_Lucene99_0");
    EXPECT_EQ(parse_segment_name("segments_1").value(), "segments_1");
    EXPECT_FALSE(parse_segment_name("").has_value());
    EXPECT_TRUE(check_file_name("_0.si", "_0").has_value());
    EXPECT_FALSE(check_file_name("_1.si", "_0").has_value());
}
