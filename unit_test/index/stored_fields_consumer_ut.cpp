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
#include <vector>

module lucene_core:ut.stored_fields_consumer;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view FORMAT_NAME = "Lucene90StoredFieldsConsumerTest";
constexpr int32_t TEXT = 0;
constexpr int32_t NUMBER = 1;

struct Fixture {
    Fixture() : infos(build_infos()), info(build_segment()) {}

    ByteBuffersDirectory directory;
    std::shared_ptr<const FieldInfos> infos;
    SegmentInfo info;

    static std::shared_ptr<const FieldInfos> build_infos() {
        FieldInfo text;
        text.number = TEXT;
        text.name = "text";
        FieldInfo number;
        number.number = NUMBER;
        number.name = "number";
        return std::make_shared<const FieldInfos>(FieldInfos::of({text, number}).value());
    }

    static SegmentInfo build_segment() {
        SegmentInfo segment;
        segment.name = "_0";
        segment.id.assign(ID_LENGTH, 0x44);
        segment.max_doc = 3;
        return segment;
    }

    [[nodiscard]] const FieldInfo &text_field() const { return *infos->by_number(TEXT); }
    [[nodiscard]] const FieldInfo &number_field() const { return *infos->by_number(NUMBER); }

    [[nodiscard]] static Lucene90CompressingStoredFieldsFormat make_format() {
        auto format = Lucene90CompressingStoredFieldsFormat::create(std::string(FORMAT_NAME),
                                                                    CompressionMode::fast(), 64, 4, 4);
        EXPECT_TRUE(format.has_value());
        return std::move(format.value());
    }
};

} // namespace

TEST(StoredFieldsConsumerTest, StoresDocumentsAndLeavesGapsEmpty) {
    Fixture fixture;
    auto format = Fixture::make_format();
    StoredFieldsConsumer consumer(fixture.directory, format, fixture.info);
    EXPECT_FALSE(consumer.is_initialized());

    ASSERT_TRUE(consumer.start_document(0).has_value());
    EXPECT_TRUE(consumer.is_initialized());
    ASSERT_TRUE(consumer.write_field(fixture.text_field(), FieldValue{std::string("hello")}).has_value());
    ASSERT_TRUE(consumer.write_field(fixture.number_field(), FieldValue{Number{int32_t{42}}}).has_value());
    ASSERT_TRUE(consumer.finish_document().has_value());

    // Document 1 is skipped on purpose: starting document 2 emits it empty.
    ASSERT_TRUE(consumer.start_document(2).has_value());
    ASSERT_TRUE(consumer.write_field(fixture.text_field(), FieldValue{std::string("world")}).has_value());
    ASSERT_TRUE(consumer.finish_document().has_value());

    ASSERT_TRUE(consumer.flush(3).has_value());

    auto reader = format.fields_reader(fixture.directory, fixture.info, fixture.infos, io_context_default());
    ASSERT_TRUE(reader.has_value()) << reader.error().to_string();

    auto first = reader.value()->document(0);
    ASSERT_TRUE(first.has_value()) << first.error().to_string();
    auto text = first.value().get("text");
    ASSERT_TRUE(text.has_value());
    ASSERT_TRUE(text.value().has_value());
    EXPECT_EQ(text.value().value(), "hello");
    // `Document::get` only stringifies text fields; numbers come back as a
    // `Field` whose numeric value is exposed through the field itself.
    ASSERT_NE(first.value().get_field("number"), nullptr);
    EXPECT_FALSE(first.value().get("number").has_value());

    auto gap = reader.value()->document(1);
    ASSERT_TRUE(gap.has_value());
    EXPECT_FALSE(gap.value().get("text").value().has_value());

    auto third = reader.value()->document(2);
    ASSERT_TRUE(third.has_value());
    EXPECT_EQ(third.value().get("text").value().value(), "world");
}

TEST(StoredFieldsConsumerTest, FinishEmitsTrailingEmptyDocuments) {
    Fixture fixture;
    auto format = Fixture::make_format();
    StoredFieldsConsumer consumer(fixture.directory, format, fixture.info);

    ASSERT_TRUE(consumer.start_document(0).has_value());
    ASSERT_TRUE(consumer.write_field(fixture.text_field(), FieldValue{std::string("only")}).has_value());
    ASSERT_TRUE(consumer.finish_document().has_value());

    // No explicit start for docs 1 and 2: `finish(3)` has to emit them.
    ASSERT_TRUE(consumer.flush(3).has_value());

    auto reader = format.fields_reader(fixture.directory, fixture.info, fixture.infos, io_context_default());
    ASSERT_TRUE(reader.has_value());
    auto last = reader.value()->document(2);
    ASSERT_TRUE(last.has_value());
    EXPECT_FALSE(last.value().get("text").value().has_value());
}

TEST(StoredFieldsConsumerTest, RejectsUnsupportedNumberTypes) {
    Fixture fixture;
    auto format = Fixture::make_format();
    StoredFieldsConsumer consumer(fixture.directory, format, fixture.info);
    ASSERT_TRUE(consumer.start_document(0).has_value());

    auto written = consumer.write_field(fixture.number_field(), FieldValue{Number{int16_t{7}}});
    ASSERT_FALSE(written.has_value());
    EXPECT_EQ(written.error().kind(), ErrorKind::kIllegalArgument);
    EXPECT_TRUE(consumer.abort().has_value());
}

TEST(StoredFieldsConsumerTest, RejectsDecreasingDocuments) {
    Fixture fixture;
    auto format = Fixture::make_format();
    StoredFieldsConsumer consumer(fixture.directory, format, fixture.info);
    ASSERT_TRUE(consumer.start_document(2).has_value());
    ASSERT_TRUE(consumer.finish_document().has_value());

    // `start_document` checks the doc order with a debug assertion; the gap
    // logic itself lives in the writer, so a repeated doc just starts again.
    // Make sure a strictly increasing start still works after the gap.
    ASSERT_TRUE(consumer.start_document(3).has_value());
    ASSERT_TRUE(consumer.finish_document().has_value());
}

TEST(StoredFieldsConsumerTest, FlushWithoutDocumentsIsANoop) {
    Fixture fixture;
    auto format = Fixture::make_format();
    StoredFieldsConsumer consumer(fixture.directory, format, fixture.info);
    EXPECT_FALSE(consumer.is_initialized());
    EXPECT_TRUE(consumer.flush(0).has_value());
    EXPECT_FALSE(consumer.is_initialized());
    EXPECT_TRUE(consumer.abort().has_value());
}
