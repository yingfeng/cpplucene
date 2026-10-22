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
#include <utility>
#include <vector>

module lucene_core:ut.push_postings_writer_base;

import lucene_core;

using namespace lucene;

namespace {

/// Two docs: doc 0 with freq 2, doc 1 with freq 1, all positions payload-bearing.
class ListPostingsEnum : public PostingsEnum {
public:
    explicit ListPostingsEnum(bool empty) : empty_(empty) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override {
        if (empty_) {
            return NO_MORE_DOCS;
        }
        ++doc_;
        position_ = -1;
        return doc_ >= 2 ? NO_MORE_DOCS : doc_;
    }
    [[nodiscard]] Result<int32_t> freq() override { return doc_ == 0 ? 2 : 1; }
    [[nodiscard]] Result<int32_t> next_position() override { return ++position_; }
    [[nodiscard]] Result<int32_t> start_offset() const override { return position_ * 10; }
    [[nodiscard]] Result<int32_t> end_offset() const override { return position_ * 10 + 5; }
    [[nodiscard]] Result<std::optional<std::span<const uint8_t>>> get_payload() const override {
        return std::optional<std::span<const uint8_t>>(std::span<const uint8_t>(payload_));
    }

private:
    bool empty_;
    int32_t doc_{-1};
    int32_t position_{-1};
    std::vector<uint8_t> payload_{0xAA, 0xBB};
};

/// Empty terms enum that hands out [`ListPostingsEnum`] and records the flags.
class StubTermsEnum : public EmptyTermsEnum {
public:
    explicit StubTermsEnum(bool empty) : empty_(empty) {}

    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>>
    postings_with_flags(std::unique_ptr<PostingsEnum> /*reuse*/, int32_t flags) override {
        last_flags = flags;
        return std::unique_ptr<PostingsEnum>(std::make_unique<ListPostingsEnum>(empty_));
    }

    int32_t last_flags{-1};

private:
    bool empty_;
};

struct PositionCall {
    int32_t position;
    bool has_payload;
    std::vector<uint8_t> payload;
    int32_t start_offset;
    int32_t end_offset;
};

struct DocCall {
    int32_t doc_id;
    int32_t freq;
    bool has_norms;
};

/// Records every push call so the wrapper's bookkeeping can be asserted.
class RecordingPushWriter : public PushPostingsWriterBaseAbstract {
public:
    [[nodiscard]] VoidResult init(IndexOutput & /*terms_out*/, const SegmentWriteState & /*state*/,
                                  const SegmentInfo & /*segment_info*/) override {
        return {};
    }

    [[nodiscard]] Result<TermStateEnum> new_term_state() override {
        return TermStateEnum{IntBlockTermState{}};
    }

    [[nodiscard]] VoidResult start_term(const FieldWriteOptions &options) override {
        start_term_options = options;
        ++start_term_calls;
        return {};
    }

    [[nodiscard]] VoidResult finish_term(TermStateEnum &state, const FieldWriteOptions & /*options*/) override {
        ++finish_term_calls;
        auto block = state.get_block_term_state();
        if (!block) {
            return err(std::move(block.error()));
        }
        finished_doc_freq = (*block)->doc_freq;
        finished_total_term_freq = (*block)->total_term_freq;
        return {};
    }

    [[nodiscard]] VoidResult start_doc(NumericDocValues *norms, int32_t doc_id, int32_t freq,
                                       const FieldWriteOptions & /*options*/) override {
        docs.push_back(DocCall{doc_id, freq, norms != nullptr});
        return {};
    }

    [[nodiscard]] VoidResult add_position(int32_t position, const BytesRef *payload, int32_t start_offset,
                                          int32_t end_offset, const FieldWriteOptions & /*options*/) override {
        PositionCall call{position, payload != nullptr, {}, start_offset, end_offset};
        if (payload != nullptr) {
            call.payload = payload->bytes();
        }
        positions.push_back(std::move(call));
        return {};
    }

    [[nodiscard]] VoidResult finish_doc() override {
        ++finish_doc_calls;
        return {};
    }

    [[nodiscard]] VoidResult encode_term_with_option(DataOutput & /*out*/, const FieldInfo & /*field_info*/,
                                                     const TermStateEnum & /*state*/, bool absolute,
                                                     const FieldWriteOptions & /*options*/) override {
        encode_absolute = absolute;
        return {};
    }

    void set_field(const FieldInfo &field_info) override {
        field = field_info;
        ++set_field_calls;
    }

    int32_t start_term_calls{0};
    int32_t finish_term_calls{0};
    int32_t finish_doc_calls{0};
    int32_t set_field_calls{0};
    int32_t finished_doc_freq{0};
    int64_t finished_total_term_freq{0};
    bool encode_absolute{false};
    FieldWriteOptions start_term_options;
    std::vector<DocCall> docs;
    std::vector<PositionCall> positions;
    FieldInfo field;
};

FieldInfo field_with(IndexOptions options, bool store_payloads = false, bool omit_norms = true) {
    FieldInfo info;
    info.name = "f";
    info.index_options = options;
    info.store_payloads = store_payloads;
    info.omit_norms = omit_norms;
    return info;
}

} // namespace

TEST(PushPostingsWriterBaseTest, SetFieldDerivesOptionsAndFlags) {
    auto sub = std::make_unique<RecordingPushWriter>();
    RecordingPushWriter *recorder = sub.get();
    PushPostingsWriterBase writer{std::move(sub)};

    writer.set_field(field_with(IndexOptions::DocsAndFreqsAndPositions, true));
    EXPECT_EQ(recorder->set_field_calls, 1);
    EXPECT_TRUE(writer.options().write_freqs);
    EXPECT_TRUE(writer.options().write_positions);
    EXPECT_TRUE(writer.options().write_payloads);
    EXPECT_FALSE(writer.options().write_offsets);
    EXPECT_EQ(writer.enum_flags(), static_cast<int32_t>(PAYLOADS));

    writer.set_field(field_with(IndexOptions::DocsAndFreqs));
    EXPECT_TRUE(writer.options().write_freqs);
    EXPECT_FALSE(writer.options().write_positions);
    EXPECT_EQ(writer.enum_flags(), static_cast<int32_t>(FREQS));

    writer.set_field(field_with(IndexOptions::Docs));
    EXPECT_FALSE(writer.options().write_freqs);
    EXPECT_EQ(writer.enum_flags(), 0);

    writer.set_field(field_with(IndexOptions::DocsAndFreqsAndPositionsAndOffsets, true));
    EXPECT_TRUE(writer.options().write_offsets);
    EXPECT_EQ(writer.enum_flags(), static_cast<int32_t>(PAYLOADS | OFFSETS));

    writer.set_field(field_with(IndexOptions::DocsAndFreqsAndPositionsAndOffsets));
    EXPECT_FALSE(writer.options().write_payloads);
    EXPECT_EQ(writer.enum_flags(), static_cast<int32_t>(OFFSETS));
}

TEST(PushPostingsWriterBaseTest, WriteTermCollectsPostings) {
    auto sub = std::make_unique<RecordingPushWriter>();
    RecordingPushWriter *recorder = sub.get();
    PushPostingsWriterBase writer{std::move(sub)};
    writer.set_field(field_with(IndexOptions::DocsAndFreqsAndPositionsAndOffsets, true));

    StubTermsEnum terms{false};
    FixedBitSet docs_seen{4};
    BytesRef term = BytesRef::from_string("ab");

    auto result = writer.write_term(term, terms, docs_seen, nullptr, nullptr);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(terms.last_flags, static_cast<int32_t>(PAYLOADS | OFFSETS));

    EXPECT_EQ(recorder->start_term_calls, 1);
    EXPECT_EQ(recorder->finish_term_calls, 1);
    EXPECT_EQ(recorder->finish_doc_calls, 2);
    ASSERT_EQ(recorder->docs.size(), 2u);
    EXPECT_EQ(recorder->docs[0].doc_id, 0);
    EXPECT_EQ(recorder->docs[0].freq, 2);
    EXPECT_FALSE(recorder->docs[0].has_norms);
    EXPECT_EQ(recorder->docs[1].freq, 1);

    // Two positions for doc 0, one for doc 1.
    ASSERT_EQ(recorder->positions.size(), 3u);
    EXPECT_EQ(recorder->positions[0].position, 0);
    EXPECT_EQ(recorder->positions[0].start_offset, 0);
    EXPECT_EQ(recorder->positions[0].end_offset, 5);
    EXPECT_TRUE(recorder->positions[0].has_payload);
    EXPECT_EQ(recorder->positions[0].payload, (std::vector<uint8_t>{0xAA, 0xBB}));
    EXPECT_EQ(recorder->positions[2].position, 0);

    EXPECT_TRUE(docs_seen.get(0).value());
    EXPECT_TRUE(docs_seen.get(1).value());
    EXPECT_FALSE(docs_seen.get(2).value());

    EXPECT_EQ(recorder->finished_doc_freq, 2);
    EXPECT_EQ(recorder->finished_total_term_freq, 3);
    ASSERT_TRUE(result.value().state.has_value());
    EXPECT_NE(result.value().postings, nullptr);
}

TEST(PushPostingsWriterBaseTest, WriteTermWithoutPositionsSkipsProximity) {
    auto sub = std::make_unique<RecordingPushWriter>();
    RecordingPushWriter *recorder = sub.get();
    PushPostingsWriterBase writer{std::move(sub)};
    writer.set_field(field_with(IndexOptions::DocsAndFreqs));

    StubTermsEnum terms{false};
    FixedBitSet docs_seen{4};

    auto result = writer.write_term(BytesRef::from_string("ab"), terms, docs_seen, nullptr, nullptr);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(terms.last_flags, static_cast<int32_t>(FREQS));
    EXPECT_TRUE(recorder->positions.empty());
    EXPECT_EQ(recorder->docs.size(), 2u);
    EXPECT_EQ(recorder->docs[1].doc_id, 1);
}

TEST(PushPostingsWriterBaseTest, WriteTermWithoutFreqsWritesMinusOne) {
    auto sub = std::make_unique<RecordingPushWriter>();
    RecordingPushWriter *recorder = sub.get();
    PushPostingsWriterBase writer{std::move(sub)};
    writer.set_field(field_with(IndexOptions::Docs));

    StubTermsEnum terms{false};
    FixedBitSet docs_seen{4};

    auto result = writer.write_term(BytesRef::from_string("ab"), terms, docs_seen, nullptr, nullptr);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(terms.last_flags, 0);
    ASSERT_EQ(recorder->docs.size(), 2u);
    EXPECT_EQ(recorder->docs[0].freq, -1);
    EXPECT_EQ(recorder->finished_doc_freq, 2);
    EXPECT_EQ(recorder->finished_total_term_freq, -1);
}

TEST(PushPostingsWriterBaseTest, WriteTermWithoutDocsSkipsTheTerm) {
    auto sub = std::make_unique<RecordingPushWriter>();
    RecordingPushWriter *recorder = sub.get();
    PushPostingsWriterBase writer{std::move(sub)};
    writer.set_field(field_with(IndexOptions::DocsAndFreqs));

    StubTermsEnum terms{true};
    FixedBitSet docs_seen{4};

    auto result = writer.write_term(BytesRef::from_string("ab"), terms, docs_seen, nullptr, nullptr);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(recorder->start_term_calls, 1);
    EXPECT_EQ(recorder->finish_term_calls, 0);
    EXPECT_TRUE(recorder->docs.empty());
    EXPECT_FALSE(result.value().state.has_value());
    EXPECT_NE(result.value().postings, nullptr);
}

TEST(PushPostingsWriterBaseTest, WriteTermRequiresFieldInfo) {
    PushPostingsWriterBase writer{std::make_unique<RecordingPushWriter>()};
    StubTermsEnum terms{false};
    FixedBitSet docs_seen{4};

    auto result = writer.write_term(BytesRef::from_string("ab"), terms, docs_seen, nullptr, nullptr);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind(), ErrorKind::kIllegalState);
}

TEST(PushPostingsWriterBaseTest, WriteTermRequiresNormsWhenFieldHasThem) {
    PushPostingsWriterBase writer{std::make_unique<RecordingPushWriter>()};
    writer.set_field(field_with(IndexOptions::DocsAndFreqs, false, false));
    StubTermsEnum terms{false};
    FixedBitSet docs_seen{4};

    auto result = writer.write_term(BytesRef::from_string("ab"), terms, docs_seen, nullptr, nullptr);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind(), ErrorKind::kIllegalState);
}
