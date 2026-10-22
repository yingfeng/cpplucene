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

module lucene_core:ut.stored_fields_merge;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view FORMAT_NAME = "Lucene90StoredFieldsMergeTest";

/// Field infos of the merged segment: one stored-only text field.
std::shared_ptr<const FieldInfos> merge_infos() {
    FieldInfo text;
    text.number = 0;
    text.name = "text";
    text.index_options = IndexOptions::None;
    return std::make_shared<const FieldInfos>(FieldInfos::of({text}).value());
}

SegmentInfo segment(std::string name, int32_t max_doc) {
    SegmentInfo info;
    info.name = std::move(name);
    info.id.assign(ID_LENGTH, 0x33);
    info.max_doc = max_doc;
    return info;
}

/// Writes `values` as segment `name` and returns the opened reader.
std::unique_ptr<Lucene90CompressingStoredFieldsReader> write_segment(ByteBuffersDirectory &directory,
                                                                    const std::string &name,
                                                                    const std::vector<std::string> &values) {
    auto format = Lucene90CompressingStoredFieldsFormat::create(std::string(FORMAT_NAME), CompressionMode::fast(),
                                                               4096, 128, 4);
    EXPECT_TRUE(format.has_value());
    if (!format.has_value()) {
        return nullptr;
    }
    SegmentInfo info = segment(name, static_cast<int32_t>(values.size()));
    auto writer = format.value().fields_writer(directory, info, io_context_default());
    EXPECT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    if (!writer.has_value()) {
        return nullptr;
    }
    auto my_infos = merge_infos();
    const FieldInfo &text = *my_infos->by_number(0);
    for (const auto &value : values) {
        EXPECT_TRUE(writer.value()->start_document().has_value());
        EXPECT_TRUE(writer.value()->write_field_str(text, value).has_value());
        EXPECT_TRUE(writer.value()->finish_document().has_value());
    }
    EXPECT_TRUE(writer.value()->finish(static_cast<int32_t>(values.size())).has_value());
    EXPECT_TRUE(writer.value()->close().has_value());

    SegmentInfo read_info = segment(name, static_cast<int32_t>(values.size()));
    auto reader = Lucene90CompressingStoredFieldsReader::create(directory, read_info, merge_infos(),
                                                                io_context_default(), FORMAT_NAME,
                                                                CompressionMode::fast());
    EXPECT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    return reader.has_value() ? std::move(reader.value()) : nullptr;
}

/// A [`StoredFields`] that fails every access; used as "this segment has none".
class NoStoredFields final : public StoredFieldsReader {
public:
    [[nodiscard]] VoidResult prefetch(int32_t /*doc_id*/) override { return {}; }
    [[nodiscard]] VoidResult document_with_visitor(int32_t /*doc_id*/, StoredFieldVisitor & /*visitor*/) override {
        return err(LuceneError::illegal_state("no stored fields"));
    }
    [[nodiscard]] Result<Document> document(int32_t /*doc_id*/) override {
        return err(LuceneError::illegal_state("no stored fields"));
    }
    [[nodiscard]] VoidResult check_integrity() const override { return {}; }
    [[nodiscard]] Result<std::unique_ptr<StoredFieldsReader>> get_merge_instance() const override {
        return std::unique_ptr<StoredFieldsReader>{};
    }
    [[nodiscard]] Result<std::unique_ptr<StoredFieldsReader>> clone() const override {
        return std::unique_ptr<StoredFieldsReader>{};
    }
    [[nodiscard]] VoidResult close() const override { return {}; }
};

/// Minimal [`CodecReader`] exposing only the stored fields reader.
class StoredFieldsCodecReader final : public CodecReader {
public:
    StoredFieldsCodecReader(StoredFieldsReader *fields, int32_t max_doc, int32_t num_docs,
                            std::shared_ptr<const Bits> live)
            : fields_(fields), max_doc_(max_doc), num_docs_(num_docs), live_(std::move(live)),
              infos_(merge_infos()) {}

    [[nodiscard]] StoredFieldsReader *get_fields_reader() override { return fields_; }
    [[nodiscard]] TermVectorsReader *get_term_vectors_reader() override { return nullptr; }
    [[nodiscard]] NormsProducer *get_norms_reader() override { return nullptr; }
    [[nodiscard]] DocValuesProducer *get_doc_values_reader() override { return nullptr; }
    [[nodiscard]] FieldsProducer *get_postings_reader() override { return nullptr; }
    [[nodiscard]] PointsReader *get_points_reader() override { return nullptr; }

    [[nodiscard]] Result<int32_t> max_doc() const override { return max_doc_; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return num_docs_; }
    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return infos_.get(); }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return live_.get(); }

private:
    StoredFieldsReader *fields_;
    int32_t max_doc_;
    int32_t num_docs_;
    std::shared_ptr<const Bits> live_;
    std::shared_ptr<const FieldInfos> infos_;
};

/// Reads the `text` field of every document of a segment.
std::vector<std::string> read_all(StoredFields &reader, int32_t num_docs) {
    std::vector<std::string> values;
    for (int32_t doc = 0; doc < num_docs; ++doc) {
        auto document = reader.document(doc);
        EXPECT_TRUE(document.has_value()) << (document ? "" : document.error().to_string());
        if (!document.has_value()) {
            continue;
        }
        auto text = document.value().get("text");
        EXPECT_TRUE(text.has_value() && text.value().has_value());
        values.push_back(text.has_value() && text.value().has_value() ? text.value().value() : std::string());
    }
    return values;
}

} // namespace

TEST(StoredFieldsMergeTest, ConcatenatesSegmentsAndSkipsDeletedDocuments) {
    ByteBuffersDirectory directory;

    auto first = write_segment(directory, "_0", {"a0", "a1", "a2"});
    ASSERT_TRUE(first != nullptr);
    auto second = write_segment(directory, "_1", {"b0", "b1"});
    ASSERT_TRUE(second != nullptr);

    // The second segment has document 1 deleted.
    auto live = std::make_shared<FixedBitSet>(2);
    live->set(0);

    StoredFieldsCodecReader reader_a(first.get(), 3, 3, nullptr);
    StoredFieldsCodecReader reader_b(second.get(), 2, 1, live);
    std::vector<CodecReader *> readers{&reader_a, &reader_b};

    SegmentInfo merged_info = segment("_2", 0);
    auto merge_state = MergeState::create(readers, merged_info, std::make_shared<InfoStreamEnum>());
    ASSERT_TRUE(merge_state.has_value()) << merge_state.error().to_string();
    EXPECT_EQ(merge_state.value()->segment_info.max_doc, 4);
    // `SegmentMerger` would compute this from the input field infos.
    merge_state.value()->merge_field_infos = merge_infos();

    auto format = Lucene90CompressingStoredFieldsFormat::create(std::string(FORMAT_NAME), CompressionMode::fast(),
                                                               4096, 128, 4);
    ASSERT_TRUE(format.has_value());
    SegmentInfo write_info = segment("_2", 0);
    auto writer = format.value().fields_writer(directory, write_info, io_context_default());
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    auto merged = writer.value()->merge(*merge_state.value());
    ASSERT_TRUE(merged.has_value()) << merged.error().to_string();
    EXPECT_EQ(merged.value(), 4);
    ASSERT_TRUE(writer.value()->close().has_value());

    SegmentInfo read_info = segment("_2", 4);
    auto reader = Lucene90CompressingStoredFieldsReader::create(directory, read_info, merge_infos(),
                                                                io_context_default(), FORMAT_NAME,
                                                                CompressionMode::fast());
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->num_docs(), 4);
    EXPECT_EQ(read_all(*reader.value(), 4), (std::vector<std::string>{"a0", "a1", "a2", "b0"}));
}

TEST(StoredFieldsMergeTest, EmptyMergeWritesNoDocuments) {
    ByteBuffersDirectory directory;

    auto first = write_segment(directory, "_0", {"a0"});
    ASSERT_TRUE(first != nullptr);
    // Both documents deleted.
    auto live = std::make_shared<FixedBitSet>(1);

    StoredFieldsCodecReader reader_a(first.get(), 1, 0, live);
    std::vector<CodecReader *> readers{&reader_a};

    SegmentInfo merged_info = segment("_1", 0);
    auto merge_state = MergeState::create(readers, merged_info, std::make_shared<InfoStreamEnum>());
    ASSERT_TRUE(merge_state.has_value());
    EXPECT_EQ(merge_state.value()->segment_info.max_doc, 0);
    merge_state.value()->merge_field_infos = merge_infos();

    auto format = Lucene90CompressingStoredFieldsFormat::create(std::string(FORMAT_NAME), CompressionMode::fast(),
                                                               4096, 128, 4);
    ASSERT_TRUE(format.has_value());
    SegmentInfo write_info = segment("_1", 0);
    auto writer = format.value().fields_writer(directory, write_info, io_context_default());
    ASSERT_TRUE(writer.has_value());

    auto merged = writer.value()->merge(*merge_state.value());
    ASSERT_TRUE(merged.has_value()) << merged.error().to_string();
    EXPECT_EQ(merged.value(), 0);
    ASSERT_TRUE(writer.value()->close().has_value());

    SegmentInfo read_info = segment("_1", 0);
    auto reader = Lucene90CompressingStoredFieldsReader::create(directory, read_info, merge_infos(),
                                                                io_context_default(), FORMAT_NAME,
                                                                CompressionMode::fast());
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->num_docs(), 0);
}

TEST(StoredFieldsMergeTest, RejectsSegmentWithoutStoredFields) {
    ByteBuffersDirectory directory;

    NoStoredFields empty;
    StoredFieldsCodecReader reader(&empty, 1, 1, nullptr);
    std::vector<CodecReader *> readers{&reader};

    SegmentInfo merged_info = segment("_0", 0);
    auto merge_state = MergeState::create(readers, merged_info, std::make_shared<InfoStreamEnum>());
    ASSERT_TRUE(merge_state.has_value());
    // A present reader is fine; the null slot is what `merge` rejects.
    merge_state.value()->stored_fields_readers[0] = nullptr;

    auto format = Lucene90CompressingStoredFieldsFormat::create(std::string(FORMAT_NAME), CompressionMode::fast(),
                                                               4096, 128, 4);
    ASSERT_TRUE(format.has_value());
    SegmentInfo write_info = segment("_0", 0);
    auto writer = format.value().fields_writer(directory, write_info, io_context_default());
    ASSERT_TRUE(writer.has_value());

    auto merged = writer.value()->merge(*merge_state.value());
    ASSERT_FALSE(merged.has_value());
    EXPECT_EQ(merged.error().kind(), ErrorKind::kIllegalState);
}
