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

module lucene_core:ut.term_vectors_merge;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view FORMAT_NAME = "Lucene90TermVectorsMergeTest";

/// One field whose vectors carry positions, offsets and payloads.
std::shared_ptr<const FieldInfos> vector_infos() {
    FieldInfo field;
    field.number = 0;
    field.name = "text";
    field.index_options = IndexOptions::DocsAndFreqsAndPositions;
    field.store_term_vector = true;
    return std::make_shared<const FieldInfos>(FieldInfos::of({field}).value());
}

SegmentInfo segment(std::string name, int32_t max_doc) {
    SegmentInfo info;
    info.name = std::move(name);
    info.id.assign(ID_LENGTH, 0x33);
    info.max_doc = max_doc;
    return info;
}

std::span<const uint8_t> bytes_of(const std::string &text) {
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

std::string text_of(const BytesRef &term) {
    return std::string(reinterpret_cast<const char *>(term.bytes().data() + term.offset()), term.length());
}

/// Writes `docs` documents, each with the two terms `a` and `b` in field `text`.
void write_segment(ByteBuffersDirectory &directory, const std::string &name, int32_t num_docs) {
    auto format = Lucene90CompressingTermVectorsFormat::create(std::string(FORMAT_NAME), "", CompressionMode::fast(),
                                                              4096, 128, 4);
    ASSERT_TRUE(format.has_value()) << (format ? "" : format.error().to_string());
    auto infos = vector_infos();
    SegmentInfo write_info = segment(name, num_docs);
    auto writer = format.value().vectors_writer(directory, write_info, io_context_default());
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    const FieldInfo &field = infos->fields()[0];
    for (int32_t doc = 0; doc < num_docs; ++doc) {
        ASSERT_TRUE(writer.value()->start_document(1).has_value());
        ASSERT_TRUE(writer.value()->start_field(field, 2, true, true, true).has_value());
        for (const std::string term : {"a", "b"}) {
            ASSERT_TRUE(writer.value()->start_term(bytes_of(term), 1).has_value());
            ASSERT_TRUE(writer.value()->add_position(0, 0, 1, std::nullopt).has_value());
        }
        ASSERT_TRUE(writer.value()->finish_field().has_value());
        ASSERT_TRUE(writer.value()->finish_document().has_value());
    }
    ASSERT_TRUE(writer.value()->finish(num_docs).has_value());
    ASSERT_TRUE(writer.value()->close().has_value());
}

std::unique_ptr<TermVectorsReader> open_reader(ByteBuffersDirectory &directory, const std::string &name,
                                              int32_t num_docs) {
    auto format = Lucene90CompressingTermVectorsFormat::create(std::string(FORMAT_NAME), "", CompressionMode::fast(),
                                                              4096, 128, 4);
    EXPECT_TRUE(format.has_value());
    auto infos = vector_infos();
    SegmentInfo read_info = segment(name, num_docs);
    auto reader = format.value().vectors_reader(directory, read_info, infos, io_context_default());
    EXPECT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    return reader.has_value() ? std::move(reader.value()) : nullptr;
}

/// The terms of one document, in vector order.
std::vector<std::string> terms_of(TermVectorsReader &reader, int32_t doc) {
    std::vector<std::string> terms;
    auto vectors = reader.get(doc);
    EXPECT_TRUE(vectors.has_value()) << (vectors ? "" : vectors.error().to_string());
    if (!vectors.has_value() || vectors.value() == nullptr) {
        return terms;
    }
    auto terms_reader = vectors.value()->terms("text");
    EXPECT_TRUE(terms_reader.has_value());
    if (!terms_reader.has_value() || terms_reader.value() == nullptr) {
        return terms;
    }
    auto iterator = terms_reader.value()->iterator();
    EXPECT_TRUE(iterator.has_value());
    if (!iterator.has_value()) {
        return terms;
    }
    for (;;) {
        auto term = iterator.value()->next();
        EXPECT_TRUE(term.has_value());
        if (!term.has_value() || term.value() == nullptr) {
            break;
        }
        terms.push_back(text_of(*term.value()));
    }
    return terms;
}

/// Minimal [`CodecReader`] exposing only a term vectors reader.
class TermVectorsCodecReader final : public CodecReader {
public:
    TermVectorsCodecReader(TermVectorsReader *vectors, int32_t max_doc, int32_t num_docs,
                           std::shared_ptr<const Bits> live, std::shared_ptr<const FieldInfos> infos)
            : vectors_(vectors), max_doc_(max_doc), num_docs_(num_docs), live_(std::move(live)),
              infos_(std::move(infos)) {}

    [[nodiscard]] StoredFieldsReader *get_fields_reader() override { return nullptr; }
    [[nodiscard]] TermVectorsReader *get_term_vectors_reader() override { return vectors_; }
    [[nodiscard]] NormsProducer *get_norms_reader() override { return nullptr; }
    [[nodiscard]] DocValuesProducer *get_doc_values_reader() override { return nullptr; }
    [[nodiscard]] FieldsProducer *get_postings_reader() override { return nullptr; }
    [[nodiscard]] PointsReader *get_points_reader() override { return nullptr; }

    [[nodiscard]] Result<int32_t> max_doc() const override { return max_doc_; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return num_docs_; }
    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return infos_.get(); }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return live_.get(); }

private:
    TermVectorsReader *vectors_;
    int32_t max_doc_;
    int32_t num_docs_;
    std::shared_ptr<const Bits> live_;
    std::shared_ptr<const FieldInfos> infos_;
};

} // namespace

TEST(TermVectorsMergeTest, ConcatenatesSegmentsAndSkipsDeletedDocuments) {
    ByteBuffersDirectory directory;
    write_segment(directory, "_0", 3);
    write_segment(directory, "_1", 2);

    auto first = open_reader(directory, "_0", 3);
    ASSERT_TRUE(first != nullptr);
    auto second = open_reader(directory, "_1", 2);
    ASSERT_TRUE(second != nullptr);

    auto live = std::make_shared<FixedBitSet>(2);
    live->set(0); // doc 1 of the second segment is deleted

    auto infos = vector_infos();
    TermVectorsCodecReader reader_a(first.get(), 3, 3, nullptr, infos);
    TermVectorsCodecReader reader_b(second.get(), 2, 1, live, infos);
    std::vector<CodecReader *> readers{&reader_a, &reader_b};

    SegmentInfo merged_info = segment("_2", 0);
    auto merge_state = MergeState::create(readers, merged_info, std::make_shared<InfoStreamEnum>());
    ASSERT_TRUE(merge_state.has_value()) << merge_state.error().to_string();
    merge_state.value()->merge_field_infos = infos;
    ASSERT_EQ(merge_state.value()->segment_info.max_doc, 4);

    auto format = Lucene90CompressingTermVectorsFormat::create(std::string(FORMAT_NAME), "", CompressionMode::fast(),
                                                              4096, 128, 4);
    ASSERT_TRUE(format.has_value());
    SegmentInfo write_info = segment("_2", 4);
    auto writer = format.value().vectors_writer(directory, write_info, io_context_default());
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    auto merged = writer.value()->merge(*merge_state.value());
    ASSERT_TRUE(merged.has_value()) << merged.error().to_string();
    EXPECT_EQ(merged.value(), 4);
    ASSERT_TRUE(writer.value()->close().has_value());

    auto reader = open_reader(directory, "_2", 4);
    ASSERT_TRUE(reader != nullptr);
    ASSERT_TRUE(reader->check_integrity().has_value());

    for (int32_t doc = 0; doc < 4; ++doc) {
        EXPECT_EQ(terms_of(*reader, doc), (std::vector<std::string>{"a", "b"})) << "doc " << doc;
    }

    // Positions and offsets survive the merge.
    auto vectors = reader->get(0);
    ASSERT_TRUE(vectors.has_value());
    auto terms = vectors.value()->terms("text");
    ASSERT_TRUE(terms.has_value());
    auto iterator = terms.value()->iterator();
    ASSERT_TRUE(iterator.has_value());
    auto term = iterator.value()->next();
    ASSERT_TRUE(term.has_value() && term.value() != nullptr);
    auto postings = iterator.value()->postings_with_flags(nullptr, OFFSETS | PAYLOADS);
    ASSERT_TRUE(postings.has_value());
    EXPECT_EQ(postings.value()->next_doc().value(), 0);
    EXPECT_EQ(postings.value()->freq().value(), 1);
    EXPECT_EQ(postings.value()->next_position().value(), 0);
    EXPECT_EQ(postings.value()->start_offset().value(), 0);
    EXPECT_EQ(postings.value()->end_offset().value(), 1);
}

TEST(TermVectorsMergeTest, EmptyMergeWritesEmptyDocumentSet) {
    ByteBuffersDirectory directory;
    write_segment(directory, "_0", 1);

    auto only = open_reader(directory, "_0", 1);
    ASSERT_TRUE(only != nullptr);

    auto live = std::make_shared<FixedBitSet>(1); // the only document is deleted
    auto infos = vector_infos();
    TermVectorsCodecReader reader(only.get(), 1, 0, live, infos);
    std::vector<CodecReader *> readers{&reader};

    SegmentInfo merged_info = segment("_1", 0);
    auto merge_state = MergeState::create(readers, merged_info, std::make_shared<InfoStreamEnum>());
    ASSERT_TRUE(merge_state.has_value());
    merge_state.value()->merge_field_infos = infos;

    auto format = Lucene90CompressingTermVectorsFormat::create(std::string(FORMAT_NAME), "", CompressionMode::fast(),
                                                              4096, 128, 4);
    ASSERT_TRUE(format.has_value());
    SegmentInfo write_info = segment("_1", 0);
    auto writer = format.value().vectors_writer(directory, write_info, io_context_default());
    ASSERT_TRUE(writer.has_value());

    auto merged = writer.value()->merge(*merge_state.value());
    ASSERT_TRUE(merged.has_value()) << merged.error().to_string();
    EXPECT_EQ(merged.value(), 0);
    ASSERT_TRUE(writer.value()->close().has_value());

    auto out = open_reader(directory, "_1", 0);
    ASSERT_TRUE(out != nullptr);
}
