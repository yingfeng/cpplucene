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
#include <string_view>
#include <vector>

module lucene_core:ut.codec_formats;

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
    info.max_doc = 4;
    return info;
}

/// A codec that only carries the three metadata formats, used to drive the
/// registry's registration path.
class MetadataOnlyCodec final : public Codec {
public:
    static constexpr std::string_view NAME = "TestMetadataOnly";

    [[nodiscard]] std::string_view name() const override { return NAME; }
    [[nodiscard]] const FieldInfosFormat &field_infos_format() const override { return field_infos_; }
    [[nodiscard]] const SegmentInfoFormat &segment_info_format() const override { return segment_info_; }
    [[nodiscard]] const LiveDocsFormat &live_docs_format() const override { return live_docs_; }

private:
    Lucene94FieldInfosFormatAdapter field_infos_{};
    Lucene99SegmentInfoFormat segment_info_{};
    Lucene90LiveDocsFormat live_docs_{};
};

} // namespace

TEST(Lucene101CodecTest, WiresEveryFormat) {
    auto created = Lucene101Codec::create();
    ASSERT_TRUE(created.has_value()) << (created ? "" : created.error().to_string());
    const Codec &codec = *created.value();

    EXPECT_EQ(codec.name(), "Lucene101");
    EXPECT_EQ(created.value()->mode(), Lucene101Codec::Mode::BestSpeed);

    // Postings and doc values go through the per-field wrappers, which is what
    // gives their files a `Lucene101_0` / `Lucene90_0` suffix.
    ASSERT_NE(codec.postings_format(), nullptr);
    EXPECT_EQ(codec.postings_format()->name(), PER_FIELD_POSTINGS_NAME);
    ASSERT_NE(codec.doc_values_format(), nullptr);
    EXPECT_EQ(codec.doc_values_format()->name(), PER_FIELD_DV_NAME);
    ASSERT_NE(codec.stored_fields_format(), nullptr);
    EXPECT_EQ(codec.stored_fields_format()->name(), "Lucene90StoredFields");
    ASSERT_NE(codec.term_vectors_format(), nullptr);
    EXPECT_EQ(codec.term_vectors_format()->name(), "Lucene90TermVectors");
    ASSERT_NE(codec.norms_format(), nullptr);
    ASSERT_NE(codec.points_format(), nullptr);
    ASSERT_NE(codec.compound_format(), nullptr);

    EXPECT_EQ(codec.field_infos_format().name(), "Lucene94FieldInfos");
    EXPECT_EQ(codec.segment_info_format().name(), "Lucene90SegmentInfo");
    EXPECT_EQ(codec.live_docs_format().name(), "Lucene90LiveDocs");
}

TEST(Lucene101CodecTest, PostingsFormatDefaultsMatchTheBlockTreeDefaults) {
    const Lucene101PostingsFormat format;
    EXPECT_EQ(format.name(), "Lucene101");
    EXPECT_EQ(format.min_term_block_size(), 25);
    EXPECT_EQ(format.max_term_block_size(), 48);

    const Lucene101PostingsFormat wide(4, 8);
    EXPECT_EQ(wide.min_term_block_size(), 4);
    EXPECT_EQ(wide.max_term_block_size(), 8);
}

TEST(Lucene90StoredFieldsFormatTest, ModeNamesRoundTrip) {
    EXPECT_EQ(Lucene90StoredFieldsFormat::mode_name(Lucene90StoredFieldsFormat::Mode::BestSpeed), "BEST_SPEED");
    EXPECT_EQ(Lucene90StoredFieldsFormat::mode_name(Lucene90StoredFieldsFormat::Mode::BestCompression),
              "BEST_COMPRESSION");

    auto speed = Lucene90StoredFieldsFormat::mode_from_name("BEST_SPEED");
    ASSERT_TRUE(speed.has_value());
    EXPECT_EQ(speed.value(), Lucene90StoredFieldsFormat::Mode::BestSpeed);

    auto high = Lucene90StoredFieldsFormat::mode_from_name("BEST_COMPRESSION");
    ASSERT_TRUE(high.has_value());
    EXPECT_EQ(high.value(), Lucene90StoredFieldsFormat::Mode::BestCompression);

    EXPECT_FALSE(Lucene90StoredFieldsFormat::mode_from_name("BEST").has_value());
}

TEST(Lucene90StoredFieldsFormatTest, BestSpeedWriterRecordsTheMode) {
    ByteBuffersDirectory directory;
    SegmentInfo info = make_segment_info();
    const Lucene90StoredFieldsFormat format(Lucene90StoredFieldsFormat::Mode::BestSpeed);

    auto writer = format.fields_writer(directory, info, io_context_default());
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    EXPECT_EQ(info.attributes.at(std::string(Lucene90StoredFieldsFormat::MODE_KEY)), "BEST_SPEED");
    ASSERT_TRUE(writer.value()->close().has_value());
}

TEST(Lucene90StoredFieldsFormatTest, BestCompressionIsRefused) {
    ByteBuffersDirectory directory;
    SegmentInfo info = make_segment_info();
    const Lucene90StoredFieldsFormat format(Lucene90StoredFieldsFormat::Mode::BestCompression);

    // The mode is persisted before the format is built, so a reader can explain
    // why the segment cannot be opened.
    auto writer = format.fields_writer(directory, info, io_context_default());
    EXPECT_FALSE(writer.has_value());
    EXPECT_EQ(info.attributes.at(std::string(Lucene90StoredFieldsFormat::MODE_KEY)), "BEST_COMPRESSION");
}

TEST(Lucene90StoredFieldsFormatTest, ConflictingModeAttributeIsRejected) {
    ByteBuffersDirectory directory;
    SegmentInfo info = make_segment_info();
    info.attributes[std::string(Lucene90StoredFieldsFormat::MODE_KEY)] = "BEST_COMPRESSION";

    const Lucene90StoredFieldsFormat format(Lucene90StoredFieldsFormat::Mode::BestSpeed);
    auto writer = format.fields_writer(directory, info, io_context_default());
    EXPECT_FALSE(writer.has_value());
    EXPECT_NE(writer.error().message().find("found existing value"), std::string::npos);
}

TEST(Lucene90StoredFieldsFormatTest, ReaderWithoutTheModeAttributeFails) {
    ByteBuffersDirectory directory;
    SegmentInfo info = make_segment_info();
    auto infos = std::make_shared<const FieldInfos>(FieldInfos::none());

    const Lucene90StoredFieldsFormat format;
    auto reader = format.fields_reader(directory, info, infos, io_context_default());
    EXPECT_FALSE(reader.has_value());
    EXPECT_NE(reader.error().message().find("missing value for"), std::string::npos);
}

TEST(FieldInfosFormatTest, AdapterRoundTripsThroughTheDirectory) {
    ByteBuffersDirectory directory;
    SegmentInfo info = make_segment_info();

    std::vector<FieldInfo> fields;
    FieldInfo body;
    body.number = 0;
    body.name = "body";
    body.index_options = IndexOptions::DocsAndFreqsAndPositions;
    fields.push_back(body);

    auto infos = FieldInfos::of(std::move(fields));
    ASSERT_TRUE(infos.has_value()) << (infos ? "" : infos.error().to_string());

    const Lucene94FieldInfosFormatAdapter format;
    EXPECT_EQ(format.name(), "Lucene94FieldInfos");
    ASSERT_TRUE(format.write(directory, info, "", infos.value(), io_context_default()).has_value());
    EXPECT_EQ(directory.file_count(), 1u);

    auto read = format.read(directory, info, "", io_context_default());
    ASSERT_TRUE(read.has_value()) << (read ? "" : read.error().to_string());
    EXPECT_EQ(read->size(), 1u);
    const FieldInfo *read_body = read->by_name("body");
    ASSERT_NE(read_body, nullptr);
    EXPECT_EQ(read_body->index_options, IndexOptions::DocsAndFreqsAndPositions);
}

TEST(CodecFormatsTest, ResolvesTheBuiltInCodecs) {
    const Codec *lucene101 = CodecFormats::for_name("Lucene101");
    ASSERT_NE(lucene101, nullptr);
    EXPECT_EQ(lucene101->name(), "Lucene101");
    EXPECT_NE(lucene101->postings_format(), nullptr);
    EXPECT_NE(lucene101->stored_fields_format(), nullptr);

    // The Lucene99 entry exists to read the `segments_N` files the older tests
    // write: metadata only.
    const Codec *lucene99 = CodecFormats::for_name("Lucene99");
    ASSERT_NE(lucene99, nullptr);
    EXPECT_EQ(lucene99->postings_format(), nullptr);
    EXPECT_EQ(lucene99->doc_values_format(), nullptr);
    EXPECT_EQ(lucene99->stored_fields_format(), nullptr);
    EXPECT_NE(&lucene99->segment_info_format(), nullptr);

    EXPECT_EQ(&CodecFormats::default_codec(), lucene101);
}

TEST(CodecFormatsTest, UnknownCodecHasNoEntry) {
    EXPECT_EQ(CodecFormats::for_name("Lucene42"), nullptr);
    EXPECT_EQ(CodecFormats::for_name(""), nullptr);
}

TEST(CodecFormatsTest, AvailableCodecsListsBothNames) {
    const auto names = CodecFormats::available_codecs();
    EXPECT_NE(std::find(names.begin(), names.end(), "Lucene101"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "Lucene99"), names.end());
}

TEST(CodecFormatsTest, RegisteredCodecBecomesResolvable) {
    auto codec = std::make_shared<MetadataOnlyCodec>();
    CodecFormats::register_codec(codec);

    const Codec *found = CodecFormats::for_name(MetadataOnlyCodec::NAME);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found, codec.get());
    EXPECT_EQ(found->postings_format(), nullptr);

    const auto names = CodecFormats::available_codecs();
    EXPECT_NE(std::find(names.begin(), names.end(), MetadataOnlyCodec::NAME), names.end());
}
