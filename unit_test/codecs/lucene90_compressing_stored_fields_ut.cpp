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
#include <variant>
#include <vector>

module lucene_core:ut.lucene90_compressing_stored_fields;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view FORMAT_NAME = "Lucene90StoredFieldsTest";

/// The field numbers the tests use.
enum FieldSlot : int32_t {
    TEXT = 0,
    BINARY = 1,
    INT32 = 2,
    INT64 = 3,
    FLOAT32 = 4,
    FLOAT64 = 5,
    COUNT = 6,
};

struct Fixture {
    Fixture() : infos(build_infos()) {}

    ByteBuffersDirectory directory;
    std::shared_ptr<const FieldInfos> infos;

    static std::shared_ptr<const FieldInfos> build_infos() {
        std::vector<FieldInfo> fields;
        for (int32_t number = 0; number < COUNT; ++number) {
            FieldInfo field;
            field.number = number;
            field.name = "f" + std::to_string(number);
            fields.push_back(std::move(field));
        }
        return std::make_shared<const FieldInfos>(FieldInfos::of(std::move(fields)).value());
    }

    [[nodiscard]] const FieldInfo &field(int32_t number) const {
        const FieldInfo *info = infos->by_number(number);
        EXPECT_TRUE(info != nullptr);
        return *info;
    }

    [[nodiscard]] SegmentInfo segment(int32_t max_doc) const {
        SegmentInfo info;
        info.name = "_0";
        info.id.assign(ID_LENGTH, 0x22);
        info.max_doc = max_doc;
        return info;
    }

    /// One document's values: text, binary, int32, int64, float, double.
    struct Values {
        std::string text;
        std::vector<uint8_t> binary;
        int32_t i32{0};
        int64_t i64{0};
        float f32{0};
        double f64{0};
    };

    static std::vector<Values> sample_values(int32_t count) {
        std::vector<Values> values;
        values.reserve(static_cast<size_t>(count));
        for (int32_t doc = 0; doc < count; ++doc) {
            Values value;
            value.text = "document-" + std::to_string(doc) + "-" + std::string(20, 'x');
            value.binary = std::vector<uint8_t>{static_cast<uint8_t>(doc), static_cast<uint8_t>(doc + 1)};
            value.i32 = doc * 7 - 3;
            value.i64 = static_cast<int64_t>(doc) * 86400000LL;  // a day in milliseconds
            value.f32 = static_cast<float>(doc) + 0.5F;
            value.f64 = -1.25 * static_cast<double>(doc) - 0.125;
            values.push_back(std::move(value));
        }
        return values;
    }

    void write(const std::vector<Values> &values, int32_t chunk_size, int32_t max_docs_per_chunk,
               int32_t block_shift = 4) {
        auto format = Lucene90CompressingStoredFieldsFormat::create(std::string(FORMAT_NAME), CompressionMode::fast(),
                                                                   chunk_size, max_docs_per_chunk, block_shift);
        ASSERT_TRUE(format.has_value()) << (format ? "" : format.error().to_string());
        SegmentInfo info = segment(static_cast<int32_t>(values.size()));
        auto writer = format.value().fields_writer(directory, info, io_context_default());
        ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

        for (const Values &value : values) {
            ASSERT_TRUE(writer.value()->start_document().has_value());
            ASSERT_TRUE(writer.value()->write_field_str(field(TEXT), value.text).has_value());
            ASSERT_TRUE(writer.value()->write_field_bytes(field(BINARY), value.binary).has_value());
            ASSERT_TRUE(writer.value()->write_field_i32(field(INT32), value.i32).has_value());
            ASSERT_TRUE(writer.value()->write_field_i64(field(INT64), value.i64).has_value());
            ASSERT_TRUE(writer.value()->write_field_f32(field(FLOAT32), value.f32).has_value());
            ASSERT_TRUE(writer.value()->write_field_f64(field(FLOAT64), value.f64).has_value());
            ASSERT_TRUE(writer.value()->finish_document().has_value());
        }
        auto finished = writer.value()->finish(static_cast<int32_t>(values.size()));
        ASSERT_TRUE(finished.has_value()) << (finished ? "" : finished.error().to_string());
        ASSERT_TRUE(writer.value()->close().has_value());
    }

    [[nodiscard]] std::unique_ptr<Lucene90CompressingStoredFieldsReader> open(int32_t num_docs,
                                                                             int32_t chunk_size = 0) {
        SegmentInfo info = segment(num_docs);
        auto reader = Lucene90CompressingStoredFieldsReader::create(
                directory, info, infos, io_context_default(), FORMAT_NAME, CompressionMode::fast());
        EXPECT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
        static_cast<void>(chunk_size);
        return reader.has_value() ? std::move(reader.value()) : nullptr;
    }
};

/// Records the fields a reader hands out, in order.
class RecordingVisitor final : public StoredFieldVisitor {
public:
    /// Load only these fields; empty means all of them.
    std::vector<int32_t> wanted;
    /// Stop after the first field.
    bool stop_early{false};

    std::vector<std::pair<int32_t, std::string>> seen;

    [[nodiscard]] Status needs_field(const FieldInfo &field_info) override {
        if (!wanted.empty() && std::find(wanted.begin(), wanted.end(), field_info.number) == wanted.end()) {
            return stop_early ? Status::Stop : Status::No;
        }
        return Status::Yes;
    }

    [[nodiscard]] VoidResult binary_field(const FieldInfo &field_info, std::span<const uint8_t> value) override {
        return record(field_info, "bin:" + std::to_string(value.size()));
    }

    [[nodiscard]] VoidResult string_field(const FieldInfo &field_info, std::string_view value) override {
        return record(field_info, "str:" + std::string(value));
    }

    [[nodiscard]] VoidResult int_field(const FieldInfo &field_info, int32_t value) override {
        return record(field_info, "i32:" + std::to_string(value));
    }

    [[nodiscard]] VoidResult long_field(const FieldInfo &field_info, int64_t value) override {
        return record(field_info, "i64:" + std::to_string(value));
    }

    [[nodiscard]] VoidResult float_field(const FieldInfo &field_info, float value) override {
        return record(field_info, "f32:" + std::to_string(value));
    }

    [[nodiscard]] VoidResult double_field(const FieldInfo &field_info, double value) override {
        return record(field_info, "f64:" + std::to_string(value));
    }

private:
    VoidResult record(const FieldInfo &field_info, std::string text) {
        seen.emplace_back(field_info.number, std::move(text));
        return {};
    }
};

} // namespace

TEST(Lucene90CompressingStoredFieldsTest, WritesAndReadsBackDocuments) {
    Fixture fixture;
    const auto values = Fixture::sample_values(3);
    fixture.write(values, /*chunk_size=*/4096, /*max_docs_per_chunk=*/128);

    auto reader = fixture.open(3);
    ASSERT_TRUE(reader != nullptr);
    EXPECT_EQ(reader->num_docs(), 3);
    EXPECT_EQ(reader->chunk_size(), 4096);

    for (int32_t doc = 0; doc < 3; ++doc) {
        auto document = reader->document(doc);
        ASSERT_TRUE(document.has_value()) << (document ? "" : document.error().to_string());
        const auto &expected = values[static_cast<size_t>(doc)];

        auto text = document.value().get("f0");
        ASSERT_TRUE(text.has_value());
        ASSERT_TRUE(text.value().has_value());
        EXPECT_EQ(text.value().value(), expected.text);

        auto binary = document.value().get_binary_value("f1");
        ASSERT_TRUE(binary.has_value());
        ASSERT_TRUE(binary.value().has_value());
        EXPECT_EQ(binary.value().value(), expected.binary);

        const IndexableField *i32 = document.value().get_field("f2");
        ASSERT_TRUE(i32 != nullptr);
        auto number = i32->numeric_value();
        ASSERT_TRUE(number.has_value());
        EXPECT_EQ(std::get<int32_t>(number.value()), expected.i32);

        const IndexableField *i64 = document.value().get_field("f3");
        ASSERT_TRUE(i64 != nullptr);
        auto long_number = i64->numeric_value();
        ASSERT_TRUE(long_number.has_value());
        EXPECT_EQ(std::get<int64_t>(long_number.value()), expected.i64);

        const IndexableField *f32 = document.value().get_field("f4");
        ASSERT_TRUE(f32 != nullptr);
        auto float_number = f32->numeric_value();
        ASSERT_TRUE(float_number.has_value());
        EXPECT_FLOAT_EQ(std::get<float>(float_number.value()), expected.f32);

        const IndexableField *f64 = document.value().get_field("f5");
        ASSERT_TRUE(f64 != nullptr);
        auto double_number = f64->numeric_value();
        ASSERT_TRUE(double_number.has_value());
        EXPECT_DOUBLE_EQ(std::get<double>(double_number.value()), expected.f64);
    }
}

TEST(Lucene90CompressingStoredFieldsTest, ReadsOnlyTheRequestedFields) {
    Fixture fixture;
    fixture.write(Fixture::sample_values(2), 4096, 128);
    auto reader = fixture.open(2);
    ASSERT_TRUE(reader != nullptr);

    RecordingVisitor visitor;
    visitor.wanted = {INT32};
    ASSERT_TRUE(reader->document_with_visitor(1, visitor).has_value());
    ASSERT_EQ(visitor.seen.size(), 1U);
    EXPECT_EQ(visitor.seen[0].first, INT32);
    EXPECT_EQ(visitor.seen[0].second, "i32:4");
}

TEST(Lucene90CompressingStoredFieldsTest, StopsAtTheFirstWantedField) {
    Fixture fixture;
    fixture.write(Fixture::sample_values(1), 4096, 128);
    auto reader = fixture.open(1);
    ASSERT_TRUE(reader != nullptr);

    RecordingVisitor visitor;
    visitor.wanted = {INT32};
    visitor.stop_early = true;
    ASSERT_TRUE(reader->document_with_visitor(0, visitor).has_value());
    // The visitor stopped on the first field it was offered, which is the text
    // one, so nothing was collected.
    EXPECT_TRUE(visitor.seen.empty());
}

TEST(Lucene90CompressingStoredFieldsTest, SpansSeveralChunks) {
    Fixture fixture;
    // A 40-byte chunk puts a couple of documents in every chunk.
    const auto values = Fixture::sample_values(10);
    fixture.write(values, /*chunk_size=*/40, /*max_docs_per_chunk=*/3);

    auto reader = fixture.open(10);
    ASSERT_TRUE(reader != nullptr);
    EXPECT_GT(reader->num_chunks(), 1);

    // Read the documents out of order so every chunk is loaded more than once.
    const std::vector<int32_t> order{9, 0, 4, 1, 8, 2, 3, 7, 5, 6};
    for (const int32_t doc : order) {
        auto document = reader->document(doc);
        ASSERT_TRUE(document.has_value()) << (document ? "" : document.error().to_string());
        auto text = document.value().get("f0");
        ASSERT_TRUE(text.has_value());
        ASSERT_TRUE(text.value().has_value());
        EXPECT_EQ(text.value().value(), values[static_cast<size_t>(doc)].text) << "doc " << doc;
    }
}

TEST(Lucene90CompressingStoredFieldsTest, CompressesSlicedChunks) {
    // A chunk is written as several compressed blocks when one document alone fills
    // more than two chunks, which is the only way the buffer grows that far: the
    // writer flushes as soon as it reaches `chunk_size`.
    Fixture fixture;
    auto values = Fixture::sample_values(1);
    values[0].text = std::string(400, 'y');

    auto format = Lucene90CompressingStoredFieldsFormat::create(std::string(FORMAT_NAME), CompressionMode::fast(),
                                                               /*chunk_size=*/16, /*max_docs_per_chunk=*/128, 4);
    ASSERT_TRUE(format.has_value());
    SegmentInfo info = fixture.segment(1);
    auto writer = format.value().fields_writer(fixture.directory, info, io_context_default());
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer.value()->start_document().has_value());
    ASSERT_TRUE(writer.value()->write_field_str(fixture.field(TEXT), values[0].text).has_value());
    ASSERT_TRUE(writer.value()->write_field_f64(fixture.field(FLOAT64), 12.5).has_value());
    ASSERT_TRUE(writer.value()->finish_document().has_value());
    ASSERT_TRUE(writer.value()->finish(1).has_value());
    ASSERT_TRUE(writer.value()->close().has_value());

    auto reader = Lucene90CompressingStoredFieldsReader::create(fixture.directory, info, fixture.infos,
                                                                io_context_default(), FORMAT_NAME,
                                                                CompressionMode::fast());
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->num_chunks(), 1);

    auto document = reader.value()->document(0);
    ASSERT_TRUE(document.has_value()) << (document ? "" : document.error().to_string());
    auto text = document.value().get("f0");
    ASSERT_TRUE(text.has_value());
    ASSERT_TRUE(text.value().has_value());
    EXPECT_EQ(text.value().value(), values[0].text);
    const IndexableField *f64 = document.value().get_field("f5");
    ASSERT_TRUE(f64 != nullptr);
    auto number = f64->numeric_value();
    ASSERT_TRUE(number.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(number.value()), 12.5);
}

TEST(Lucene90CompressingStoredFieldsTest, ReportsDirtyChunks) {
    Fixture fixture;
    // Two full chunks of two documents each, then a last chunk that only `finish`
    // can complete: that one is the dirty chunk.
    fixture.write(Fixture::sample_values(5), /*chunk_size=*/300, /*max_docs_per_chunk=*/2);
    auto reader = fixture.open(5);
    ASSERT_TRUE(reader != nullptr);
    EXPECT_EQ(reader->num_chunks(), 3);
    EXPECT_EQ(reader->num_dirty_chunks(), 1);
    EXPECT_EQ(reader->num_dirty_docs(), 1);
}

TEST(Lucene90CompressingStoredFieldsTest, PrefetchesAndChecksIntegrity) {
    Fixture fixture;
    fixture.write(Fixture::sample_values(4), 64, 2);
    auto reader = fixture.open(4);
    ASSERT_TRUE(reader != nullptr);

    EXPECT_TRUE(reader->prefetch(3).has_value());
    auto document = reader->document(3);
    ASSERT_TRUE(document.has_value());

    EXPECT_TRUE(reader->check_integrity().has_value());
}

TEST(Lucene90CompressingStoredFieldsTest, ClonesIntoAnIndependentReader) {
    Fixture fixture;
    const auto values = Fixture::sample_values(3);
    fixture.write(values, 64, 2);

    auto reader = fixture.open(3);
    ASSERT_TRUE(reader != nullptr);
    auto copy = reader->clone();
    ASSERT_TRUE(copy.has_value()) << (copy ? "" : copy.error().to_string());

    ASSERT_TRUE(reader->close().has_value());
    auto document = copy.value()->document(2);
    ASSERT_TRUE(document.has_value()) << (document ? "" : document.error().to_string());
    auto text = document.value().get("f0");
    ASSERT_TRUE(text.has_value());
    ASSERT_TRUE(text.value().has_value());
    EXPECT_EQ(text.value().value(), values[2].text);
}

TEST(Lucene90CompressingStoredFieldsTest, HandlesEmptyDocuments) {
    Fixture fixture;
    auto format = Lucene90CompressingStoredFieldsFormat::create(std::string(FORMAT_NAME), CompressionMode::fast(), 4096, 128, 4);
    ASSERT_TRUE(format.has_value());
    SegmentInfo info = fixture.segment(2);
    auto writer = format.value().fields_writer(fixture.directory, info, io_context_default());
    ASSERT_TRUE(writer.has_value());

    // A document with no stored fields, then one with a single field.
    ASSERT_TRUE(writer.value()->start_document().has_value());
    ASSERT_TRUE(writer.value()->finish_document().has_value());
    ASSERT_TRUE(writer.value()->start_document().has_value());
    ASSERT_TRUE(writer.value()->write_field_str(fixture.field(TEXT), "only").has_value());
    ASSERT_TRUE(writer.value()->finish_document().has_value());
    ASSERT_TRUE(writer.value()->finish(2).has_value());
    ASSERT_TRUE(writer.value()->close().has_value());

    auto reader = fixture.open(2);
    ASSERT_TRUE(reader != nullptr);
    auto empty = reader->document(0);
    ASSERT_TRUE(empty.has_value()) << (empty ? "" : empty.error().to_string());
    EXPECT_TRUE(empty.value().empty());

    auto single = reader->document(1);
    ASSERT_TRUE(single.has_value());
    auto text = single.value().get("f0");
    ASSERT_TRUE(text.has_value());
    ASSERT_TRUE(text.value().has_value());
    EXPECT_EQ(text.value().value(), "only");
}

TEST(Lucene90CompressingStoredFieldsTest, RejectsAWrongDocumentCount) {
    Fixture fixture;
    auto format = Lucene90CompressingStoredFieldsFormat::create(std::string(FORMAT_NAME), CompressionMode::fast(), 4096, 128, 4);
    ASSERT_TRUE(format.has_value());
    SegmentInfo info = fixture.segment(2);
    auto writer = format.value().fields_writer(fixture.directory, info, io_context_default());
    ASSERT_TRUE(writer.has_value());

    ASSERT_TRUE(writer.value()->start_document().has_value());
    ASSERT_TRUE(writer.value()->finish_document().has_value());
    // Only one document was written, so claiming two must fail.
    EXPECT_FALSE(writer.value()->finish(2).has_value());
    ASSERT_TRUE(writer.value()->close().has_value());
}

TEST(Lucene90CompressingStoredFieldsTest, ExposesTheFormatFactories) {
    auto format = Lucene90CompressingStoredFieldsFormat::create(std::string(FORMAT_NAME), CompressionMode::fast(), 512, 32, 4);
    ASSERT_TRUE(format.has_value());
    EXPECT_EQ(format.value().name(), FORMAT_NAME);
    EXPECT_EQ(format.value().chunk_size(), 512);
    EXPECT_EQ(format.value().max_docs_per_chunk(), 32);
    EXPECT_EQ(format.value().block_shift(), 4);
    EXPECT_EQ(format.value().compression_mode().name(), "FAST");

    // The reader and the writer go through the format interface.
    Fixture fixture;
    SegmentInfo info = fixture.segment(1);
    auto writer = format.value().fields_writer(fixture.directory, info, io_context_default());
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer.value()->start_document().has_value());
    ASSERT_TRUE(writer.value()->write_field_i64(fixture.field(INT64), 17179869184LL).has_value());
    ASSERT_TRUE(writer.value()->finish_document().has_value());
    ASSERT_TRUE(writer.value()->finish(1).has_value());
    ASSERT_TRUE(writer.value()->close().has_value());

    auto reader = format.value().fields_reader(fixture.directory, info, fixture.infos, io_context_default());
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    auto document = reader.value()->document(0);
    ASSERT_TRUE(document.has_value());
    const IndexableField *field = document.value().get_field("f3");
    ASSERT_TRUE(field != nullptr);
    auto number = field->numeric_value();
    ASSERT_TRUE(number.has_value());
    EXPECT_EQ(std::get<int64_t>(number.value()), 17179869184LL);
}

TEST(Lucene90CompressingStoredFieldsTest, RejectsInvalidParameters) {
    EXPECT_FALSE(Lucene90CompressingStoredFieldsFormat::create(std::string(FORMAT_NAME), CompressionMode::fast(), 0, 32, 4)
                         .has_value());
    EXPECT_FALSE(Lucene90CompressingStoredFieldsFormat::create(std::string(FORMAT_NAME), CompressionMode::fast(), 512, 0, 4)
                         .has_value());
    EXPECT_FALSE(Lucene90CompressingStoredFieldsFormat::create(std::string(FORMAT_NAME), CompressionMode::fast(), 512, 32, 1)
                         .has_value());
}
