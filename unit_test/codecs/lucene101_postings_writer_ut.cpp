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
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.lucene101_postings_writer;

import lucene_core;

using namespace lucene;

namespace {

FieldInfo make_field(IndexOptions options, bool store_payloads = false) {
    FieldInfo info;
    info.number = 0;
    info.name = "f";
    info.index_options = options;
    info.store_payloads = store_payloads;
    info.omit_norms = true;
    return info;
}

std::shared_ptr<const FieldInfos> field_infos_with(const FieldInfo &field) {
    auto infos = FieldInfos::of({field});
    return std::make_shared<const FieldInfos>(std::move(infos.value()));
}

SegmentWriteState write_state(Directory &directory, std::shared_ptr<const FieldInfos> infos,
                              std::string suffix = {}) {
    return SegmentWriteState{get_default_info_stream(), directory, std::move(infos), io_context_default(),
                             std::move(suffix)};
}

SegmentInfo segment_info_for(std::string name = "_0") {
    SegmentInfo info;
    info.name = std::move(name);
    info.id.assign(ID_LENGTH, 0xAB);
    return info;
}

std::string list_files(const Directory &directory) {
    std::string joined;
    auto files = directory.list_all();
    if (!files.has_value()) {
        return "<error>";
    }
    for (const auto &file : files.value()) {
        joined += file;
        joined += ' ';
    }
    return joined;
}

bool has_file(const Directory &directory, std::string_view name) {
    auto files = directory.list_all();
    if (!files.has_value()) {
        return false;
    }
    for (const auto &file : files.value()) {
        if (file == name) {
            return true;
        }
    }
    return false;
}

/// Two docs (0 and 3) with frequencies 2 and 1, all positions payload-bearing.
class ListPostingsEnum : public PostingsEnum {
public:
    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override {
        ++doc_;
        position_ = -1;
        return doc_ >= 2 ? NO_MORE_DOCS : (doc_ == 0 ? 0 : 3);
    }
    [[nodiscard]] Result<int32_t> freq() override { return doc_ == 0 ? 2 : 1; }
    [[nodiscard]] Result<int32_t> next_position() override { return ++position_; }
    [[nodiscard]] Result<int32_t> start_offset() const override { return position_ * 10; }
    [[nodiscard]] Result<int32_t> end_offset() const override { return position_ * 10 + 5; }
    [[nodiscard]] Result<std::optional<std::span<const uint8_t>>> get_payload() const override {
        return std::optional<std::span<const uint8_t>>(std::span<const uint8_t>(payload_));
    }

private:
    int32_t doc_{-1};
    int32_t position_{-1};
    std::vector<uint8_t> payload_{0xAA, 0xBB};
};

class StubTermsEnum : public EmptyTermsEnum {
public:
    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>>
    postings_with_flags(std::unique_ptr<PostingsEnum> /*reuse*/, int32_t /*flags*/) override {
        return std::unique_ptr<PostingsEnum>(std::make_unique<ListPostingsEnum>());
    }
};

} // namespace

TEST(Lucene101PostingsWriterTest, CreatesFilesWithoutProximity) {
    ByteBuffersDirectory directory;
    FieldInfo field = make_field(IndexOptions::DocsAndFreqs);
    auto state = write_state(directory, field_infos_with(field));
    auto segment = segment_info_for();

    auto writer = Lucene101PostingsWriter::create(state, segment);
    ASSERT_TRUE(writer.has_value());

    // `ByteBuffersDirectory` only publishes a file once its output is closed.
    ASSERT_TRUE(writer.value()->close().has_value());
    EXPECT_TRUE(writer.value()->is_closed());
    EXPECT_TRUE(has_file(directory, "_0.psm"));
    EXPECT_TRUE(has_file(directory, "_0.doc"));
    EXPECT_FALSE(has_file(directory, "_0.pos"));
    EXPECT_FALSE(has_file(directory, "_0.pay"));
    EXPECT_GT(directory.file_length("_0.psm").value(), size_t{0});
    EXPECT_GT(directory.file_length("_0.doc").value(), size_t{0});
}

TEST(Lucene101PostingsWriterTest, CreatesProximityFilesWhenFieldInfosSaySo) {
    ByteBuffersDirectory directory;
    FieldInfo field = make_field(IndexOptions::DocsAndFreqsAndPositionsAndOffsets, true);
    auto state = write_state(directory, field_infos_with(field));
    auto segment = segment_info_for();

    auto writer = Lucene101PostingsWriter::create(state, segment);
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer.value()->close().has_value());
    EXPECT_TRUE(has_file(directory, "_0.pos"));
    EXPECT_TRUE(has_file(directory, "_0.pay"));
}

TEST(Lucene101PostingsWriterTest, WritesMultiDocTerm) {
    ByteBuffersDirectory directory;
    FieldInfo field = make_field(IndexOptions::DocsAndFreqs);
    auto state = write_state(directory, field_infos_with(field));
    auto segment = segment_info_for();

    auto writer = Lucene101PostingsWriter::create(state, segment);
    ASSERT_TRUE(writer.has_value());
    Lucene101PostingsWriter &postings = *writer.value();
    postings.set_field(field);

    FieldWriteOptions options;
    options.write_freqs = true;
    ASSERT_TRUE(postings.start_term(options).has_value());
    ASSERT_TRUE(postings.start_doc(nullptr, 0, 2, options).has_value());
    ASSERT_TRUE(postings.finish_doc().has_value());
    ASSERT_TRUE(postings.start_doc(nullptr, 3, 1, options).has_value());
    ASSERT_TRUE(postings.finish_doc().has_value());
    EXPECT_EQ(postings.doc_count(), 2);

    auto term_state = postings.new_term_state();
    ASSERT_TRUE(term_state.has_value());
    auto block_state = term_state.value().get_block_term_state_mut();
    ASSERT_TRUE(block_state.has_value());
    (*block_state)->doc_freq = 2;
    (*block_state)->total_term_freq = 3;

    ASSERT_TRUE(postings.finish_term(term_state.value(), options).has_value());
    auto *int_state = std::get_if<IntBlockTermState>(&term_state.value().value());
    ASSERT_NE(int_state, nullptr);
    // The doc file already contains the codec header when the term starts.
    EXPECT_GT(int_state->doc_start_fp, 0);
    EXPECT_EQ(int_state->singleton_doc_id, -1);
    EXPECT_EQ(int_state->base.doc_freq, 2);

    GrowableByteArrayDataOutput meta;
    ASSERT_TRUE(postings.encode_term_with_option(meta, field, term_state.value(), true, options).has_value());
    EXPECT_GT(meta.size(), size_t{0});
    // Not pulsed: `(docStartFP - lastDocStartFP) << 1`, so the low bit is clear.
    EXPECT_EQ(meta.bytes()[0] & 1, 0);

    ASSERT_TRUE(postings.close().has_value());
    EXPECT_TRUE(postings.is_closed());
    // A second close is a no-op.
    EXPECT_TRUE(postings.close().has_value());
}

TEST(Lucene101PostingsWriterTest, SingleDocTermIsPulsed) {
    ByteBuffersDirectory directory;
    FieldInfo field = make_field(IndexOptions::DocsAndFreqs);
    auto state = write_state(directory, field_infos_with(field));
    auto segment = segment_info_for();

    auto writer = Lucene101PostingsWriter::create(state, segment);
    ASSERT_TRUE(writer.has_value());
    Lucene101PostingsWriter &postings = *writer.value();
    postings.set_field(field);

    FieldWriteOptions options;
    options.write_freqs = true;
    ASSERT_TRUE(postings.start_term(options).has_value());
    ASSERT_TRUE(postings.start_doc(nullptr, 0, 1, options).has_value());
    ASSERT_TRUE(postings.finish_doc().has_value());

    auto term_state = postings.new_term_state();
    ASSERT_TRUE(term_state.has_value());
    auto block_state = term_state.value().get_block_term_state_mut();
    ASSERT_TRUE(block_state.has_value());
    (*block_state)->doc_freq = 1;
    (*block_state)->total_term_freq = 1;
    ASSERT_TRUE(postings.finish_term(term_state.value(), options).has_value());

    auto *int_state = std::get_if<IntBlockTermState>(&term_state.value().value());
    ASSERT_NE(int_state, nullptr);
    EXPECT_EQ(int_state->singleton_doc_id, 0);
    EXPECT_GT(int_state->doc_start_fp, 0);

    // The very first term has no previous state, so it takes the regular form.
    GrowableByteArrayDataOutput first;
    ASSERT_TRUE(postings.encode_term_with_option(first, field, term_state.value(), true, options).has_value());
    ASSERT_GT(first.size(), size_t{0});
    EXPECT_EQ(first.bytes()[0] & 1, 0);

    // A second pulsed term with the same `docStartFP` uses the delta form:
    // `(zigZag(docIdDelta) << 1) | 1`, i.e. an odd low bit.
    ASSERT_TRUE(postings.start_term(options).has_value());
    ASSERT_TRUE(postings.start_doc(nullptr, 0, 1, options).has_value());
    ASSERT_TRUE(postings.finish_doc().has_value());
    auto second_state = postings.new_term_state();
    ASSERT_TRUE(second_state.has_value());
    auto second_block = second_state.value().get_block_term_state_mut();
    ASSERT_TRUE(second_block.has_value());
    (*second_block)->doc_freq = 1;
    (*second_block)->total_term_freq = 1;
    ASSERT_TRUE(postings.finish_term(second_state.value(), options).has_value());

    GrowableByteArrayDataOutput second;
    ASSERT_TRUE(postings.encode_term_with_option(second, field, second_state.value(), false, options).has_value());
    ASSERT_GT(second.size(), size_t{0});
    EXPECT_EQ(second.bytes()[0] & 1, 1);

    ASSERT_TRUE(postings.close().has_value());
}

TEST(Lucene101PostingsWriterTest, AddPositionValidatesPosition) {
    ByteBuffersDirectory directory;
    FieldInfo field = make_field(IndexOptions::DocsAndFreqsAndPositions, true);
    auto state = write_state(directory, field_infos_with(field));
    auto segment = segment_info_for();

    auto writer = Lucene101PostingsWriter::create(state, segment);
    ASSERT_TRUE(writer.has_value());
    Lucene101PostingsWriter &postings = *writer.value();
    postings.set_field(field);

    FieldWriteOptions options;
    options.write_freqs = true;
    options.write_positions = true;
    options.write_payloads = true;
    ASSERT_TRUE(postings.start_term(options).has_value());

    auto negative = postings.add_position(-1, nullptr, -1, -1, options);
    ASSERT_FALSE(negative.has_value());
    EXPECT_EQ(negative.error().kind(), ErrorKind::kCorruptIndex);

    auto too_large = postings.add_position(MAX_POSITION + 1, nullptr, -1, -1, options);
    ASSERT_FALSE(too_large.has_value());
    EXPECT_EQ(too_large.error().kind(), ErrorKind::kCorruptIndex);

    ASSERT_TRUE(postings.add_position(0, nullptr, 0, 5, options).has_value());
    ASSERT_TRUE(postings.close().has_value());
}

TEST(Lucene101PostingsWriterTest, WorksThroughThePushWrapper) {
    ByteBuffersDirectory directory;
    FieldInfo field = make_field(IndexOptions::DocsAndFreqsAndPositionsAndOffsets, true);
    auto state = write_state(directory, field_infos_with(field), "1");
    auto segment = segment_info_for();

    auto writer = Lucene101PostingsWriter::create(state, segment);
    ASSERT_TRUE(writer.has_value());

    PushPostingsWriterBase pushed{std::move(writer.value())};
    pushed.set_field(field);
    EXPECT_TRUE(pushed.options().write_positions);

    StubTermsEnum terms;
    FixedBitSet docs_seen{8};
    auto result = pushed.write_term(BytesRef::from_string("ab"), terms, docs_seen, nullptr, nullptr);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result.value().state.has_value());
    auto *int_state = std::get_if<IntBlockTermState>(&result.value().state->value());
    ASSERT_NE(int_state, nullptr);
    EXPECT_EQ(int_state->base.doc_freq, 2);
    EXPECT_EQ(int_state->base.total_term_freq, 3);
    EXPECT_TRUE(docs_seen.get(0).value());
    EXPECT_TRUE(docs_seen.get(3).value());
    EXPECT_FALSE(docs_seen.get(2).value());

    EXPECT_TRUE(pushed.close().has_value());
    EXPECT_TRUE(has_file(directory, "_0_1.pos")) << "files: " << list_files(directory);
    EXPECT_TRUE(has_file(directory, "_0_1.pay")) << "files: " << list_files(directory);
}
