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
#include <limits>
#include <optional>
#include <span>
#include <vector>

module lucene_core:ut.lucene101_postings_reader;

import lucene_core;

using namespace lucene;

namespace {

std::vector<Impact> write_and_read_impacts(const std::vector<Impact> &impacts) {
    GrowableByteArrayDataOutput out;
    EXPECT_TRUE(write_impacts(impacts, out).has_value());
    auto decoded = ImpactsImpl::decode(out.bytes(), impacts.size());
    EXPECT_TRUE(decoded.has_value());
    decoded.value().impacts.resize(decoded.value().length);
    return decoded.value().impacts;
}

} // namespace

TEST(PostingsReaderHelpersTest, Vint15RoundTrip) {
    const std::vector<int32_t> values{0, 1, 0x7FFF - 1, 0x7FFF, 0x8000, 100000, 1 << 20,
                                      std::numeric_limits<int32_t>::max() - 128};
    GrowableByteArrayDataOutput out;
    for (int32_t value : values) {
        ASSERT_TRUE(write_vint15(out, value).has_value());
    }
    // Every value takes its 2-byte prefix, plus a vlong for the high bits.
    EXPECT_GE(out.size(), 2u * values.size());

    ByteArrayDataInput in{out.bytes()};
    for (int32_t value : values) {
        auto read = read_vint15(in);
        ASSERT_TRUE(read.has_value());
        EXPECT_EQ(read.value(), value);
    }
    EXPECT_EQ(in.get_position(), in.length());
}

TEST(PostingsReaderHelpersTest, Vlong15RoundTrip) {
    const std::vector<int64_t> values{0, 3, 0x7FFE, 0x7FFF, 0x8000, int64_t{1} << 40};
    GrowableByteArrayDataOutput out;
    for (int64_t value : values) {
        ASSERT_TRUE(write_vlong15(out, value).has_value());
    }

    ByteArrayDataInput in{out.bytes()};
    for (int64_t value : values) {
        auto read = read_vlong15(in);
        ASSERT_TRUE(read.has_value());
        EXPECT_EQ(read.value(), value);
    }
    EXPECT_EQ(in.get_position(), in.length());
}

TEST(PostingsReaderHelpersTest, ImpactsRoundTrip) {
    const std::vector<Impact> impacts{Impact{1, 1}, Impact{3, 4}, Impact{7, 16}};
    const std::vector<Impact> decoded = write_and_read_impacts(impacts);
    ASSERT_EQ(decoded.size(), impacts.size());
    for (size_t i = 0; i < impacts.size(); ++i) {
        EXPECT_EQ(decoded[i].freq, impacts[i].freq);
        EXPECT_EQ(decoded[i].norm, impacts[i].norm);
    }
}

TEST(PostingsReaderHelpersTest, ImpactsWithConsecutiveNormsUseShortForm) {
    // norm grows by exactly one each time, so the norm delta folds into the
    // first vint: one byte per impact.
    const std::vector<Impact> impacts{Impact{1, 1}, Impact{2, 2}, Impact{3, 3}};
    GrowableByteArrayDataOutput out;
    ASSERT_TRUE(write_impacts(impacts, out).has_value());
    EXPECT_EQ(out.size(), impacts.size());
    EXPECT_EQ(write_and_read_impacts(impacts).size(), impacts.size());
}

TEST(PostingsReaderHelpersTest, PrefixSumAndSumOverRange) {
    std::vector<int32_t> values{1, 2, 3, 4};
    prefix_sum(values, values.size(), 10);
    EXPECT_EQ(values[0], 11);
    EXPECT_EQ(values[1], 13);
    EXPECT_EQ(values[2], 16);
    EXPECT_EQ(values[3], 20);
    EXPECT_EQ(sum_over_range(std::span<const int32_t>(values), 1, 3), 29);
    EXPECT_EQ(sum_over_range(std::span<const int32_t>(values), 0, 0), 0);
}

TEST(ImpactsImplTest, WithoutFreqsThereIsOneFallbackLevel) {
    ImpactsImpl impacts{false, 10, NO_MORE_DOCS, std::nullopt, std::nullopt, 0, 0};
    EXPECT_EQ(impacts.num_levels(), 1u);
    EXPECT_EQ(impacts.get_doc_id_upto(0), NO_MORE_DOCS);
    EXPECT_EQ(impacts.get_doc_id_upto(1), NO_MORE_DOCS);

    auto values = impacts.get_impacts(0);
    ASSERT_TRUE(values.has_value());
    ASSERT_EQ(values.value().size(), 1u);
    EXPECT_EQ(values.value()[0].freq, std::numeric_limits<int32_t>::max());
    EXPECT_EQ(values.value()[0].norm, 1);
}

TEST(ImpactsImplTest, DecodesSerializedLevels) {
    const std::vector<Impact> level0_impacts{Impact{2, 3}, Impact{4, 9}};
    GrowableByteArrayDataOutput level0;
    ASSERT_TRUE(write_impacts(level0_impacts, level0).has_value());
    const std::vector<Impact> level1_impacts{Impact{1, 1}};
    GrowableByteArrayDataOutput level1;
    ASSERT_TRUE(write_impacts(level1_impacts, level1).has_value());

    ImpactsImpl single{true, 10, NO_MORE_DOCS, std::span<const uint8_t>(level0.bytes()), std::nullopt, 4, 8};
    EXPECT_EQ(single.num_levels(), 1u);
    EXPECT_EQ(single.get_doc_id_upto(0), 10);
    auto level0_values = single.get_impacts(0);
    ASSERT_TRUE(level0_values.has_value());
    ASSERT_EQ(level0_values.value().size(), 2u);
    EXPECT_EQ(level0_values.value()[0].norm, 3);
    EXPECT_EQ(level0_values.value()[1].freq, 4);

    // Level 1 was not provided at all, which is an error for a two-level term.
    auto missing = single.get_impacts(1);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().kind(), ErrorKind::kIllegalState);

    ImpactsImpl both{true, 10, 100, std::span<const uint8_t>(level0.bytes()), std::span<const uint8_t>(level1.bytes()),
                     4, 8};
    EXPECT_EQ(both.num_levels(), 2u);
    EXPECT_EQ(both.get_doc_id_upto(1), 100);
    EXPECT_EQ(both.get_doc_id_upto(2), NO_MORE_DOCS);
    auto level1_values = both.get_impacts(1);
    ASSERT_TRUE(level1_values.has_value());
    ASSERT_EQ(level1_values.value().size(), 1u);
    EXPECT_EQ(level1_values.value()[0].freq, 1);
}

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

SegmentInfo segment_info_for(std::string name = "_0", uint8_t id_byte = 0xAB) {
    SegmentInfo info;
    info.name = std::move(name);
    info.id.assign(ID_LENGTH, id_byte);
    return info;
}

/// Writes one term over the two docs 0 and 3 (freqs 2 and 1) and returns its
/// encoded term metadata.
struct WrittenTerm {
    std::vector<uint8_t> encoded;
    int64_t doc_start_fp{0};
    int32_t doc_freq{2};
    int64_t total_term_freq{3};
};

Result<WrittenTerm> write_term(Directory &directory, std::shared_ptr<const FieldInfos> infos,
                               const SegmentInfo &segment, const FieldInfo &field,
                               const FieldWriteOptions &options) {
    SegmentWriteState state{get_default_info_stream(), directory, std::move(infos), io_context_default(),
                            segment.name == "_0" ? std::string{} : std::string{}};
    auto writer = Lucene101PostingsWriter::create(state, segment);
    if (!writer) {
        return err(writer.error());
    }
    Lucene101PostingsWriter &postings = *writer.value();
    postings.set_field(field);
    if (auto started = postings.start_term(options); !started) {
        return err(started.error());
    }
    if (auto started = postings.start_doc(nullptr, 0, 2, options); !started) {
        return err(started.error());
    }
    if (auto finished = postings.finish_doc(); !finished) {
        return err(finished.error());
    }
    if (auto started = postings.start_doc(nullptr, 3, 1, options); !started) {
        return err(started.error());
    }
    if (auto finished = postings.finish_doc(); !finished) {
        return err(finished.error());
    }
    if (options.write_positions) {
        if (auto added = postings.add_position(0, nullptr, 0, 5, options); !added) {
            return err(added.error());
        }
    }

    auto term_state = postings.new_term_state();
    if (!term_state) {
        return err(term_state.error());
    }
    auto block = term_state.value().get_block_term_state_mut();
    if (!block) {
        return err(block.error());
    }
    (*block)->doc_freq = 2;
    (*block)->total_term_freq = 3;
    if (auto finished = postings.finish_term(term_state.value(), options); !finished) {
        return err(finished.error());
    }

    GrowableByteArrayDataOutput meta;
    if (auto encoded = postings.encode_term_with_option(meta, field, term_state.value(), true, options);
        !encoded) {
        return err(encoded.error());
    }
    const auto *int_state = std::get_if<IntBlockTermState>(&term_state.value().value());
    if (int_state == nullptr) {
        return err(LuceneError::illegal_state("expected IntBlockTermState"));
    }
    WrittenTerm written;
    written.encoded = meta.take_bytes();
    written.doc_start_fp = int_state->doc_start_fp;
    if (auto closed = postings.close(); !closed) {
        return err(closed.error());
    }
    return written;
}

} // namespace

TEST(Lucene101PostingsReaderTest, OpensWhatTheWriterWrote) {
    ByteBuffersDirectory directory;
    FieldInfo field = make_field(IndexOptions::DocsAndFreqs);
    auto infos = field_infos_with(field);
    auto segment = segment_info_for();

    FieldWriteOptions options;
    options.write_freqs = true;
    auto written = write_term(directory, infos, segment, field, options);
    ASSERT_TRUE(written.has_value());

    SegmentReadState read_state{directory, infos, io_context_default()};
    auto reader = Lucene101PostingsReader::create(read_state, segment);
    ASSERT_TRUE(reader.has_value());
    EXPECT_FALSE(reader.value()->has_positions());
    EXPECT_FALSE(reader.value()->has_payloads());
    EXPECT_EQ(reader.value()->max_num_impacts_at_level0(), 0);
    EXPECT_EQ(reader.value()->to_string(), "Lucene101PostingsReader(positions=false,payloads=false)");
    auto integrity = reader.value()->check_integrity();
    ASSERT_TRUE(integrity.has_value()) << integrity.error().to_string();

    // Decode the metadata the writer produced.
    auto term_state = reader.value()->new_term_state();
    ASSERT_TRUE(term_state.has_value());
    auto block = term_state.value().get_block_term_state_mut();
    ASSERT_TRUE(block.has_value());
    (*block)->doc_freq = 2;
    (*block)->total_term_freq = 3;

    ByteArrayDataInput meta_in{written.value().encoded};
    ASSERT_TRUE(reader.value()->decode_term(meta_in, field, term_state.value(), true).has_value());
    const auto *decoded = std::get_if<IntBlockTermState>(&term_state.value().value());
    ASSERT_NE(decoded, nullptr);
    EXPECT_EQ(decoded->doc_start_fp, written.value().doc_start_fp);
    EXPECT_EQ(decoded->singleton_doc_id, -1);
    EXPECT_EQ(meta_in.get_position(), meta_in.length());

    ASSERT_TRUE(reader.value()->close().has_value());
    EXPECT_TRUE(reader.value()->close().has_value());
}

TEST(Lucene101PostingsReaderTest, OpensProximityFiles) {
    ByteBuffersDirectory directory;
    FieldInfo field = make_field(IndexOptions::DocsAndFreqsAndPositionsAndOffsets, true);
    auto infos = field_infos_with(field);
    auto segment = segment_info_for();

    FieldWriteOptions options;
    options.write_freqs = true;
    options.write_positions = true;
    options.write_payloads = true;
    options.write_offsets = true;
    ASSERT_TRUE(write_term(directory, infos, segment, field, options).has_value());

    SegmentReadState read_state{directory, infos, io_context_default()};
    auto reader = Lucene101PostingsReader::create(read_state, segment);
    ASSERT_TRUE(reader.has_value());
    EXPECT_TRUE(reader.value()->has_positions());
    EXPECT_TRUE(reader.value()->has_payloads());
    auto integrity = reader.value()->check_integrity();
    ASSERT_TRUE(integrity.has_value()) << integrity.error().to_string();
    ASSERT_TRUE(reader.value()->close().has_value());
}

TEST(Lucene101PostingsReaderTest, RejectsMismatchedSegmentId) {
    ByteBuffersDirectory directory;
    FieldInfo field = make_field(IndexOptions::DocsAndFreqs);
    auto infos = field_infos_with(field);
    auto segment = segment_info_for();

    FieldWriteOptions options;
    options.write_freqs = true;
    ASSERT_TRUE(write_term(directory, infos, segment, field, options).has_value());

    SegmentReadState read_state{directory, infos, io_context_default()};
    auto wrong_segment = segment_info_for("_0", 0xCD);
    auto reader = Lucene101PostingsReader::create(read_state, wrong_segment);
    ASSERT_FALSE(reader.has_value());
    EXPECT_EQ(reader.error().kind(), ErrorKind::kCorruptIndex);
}

TEST(Lucene101PostingsReaderTest, DecodesPulsedTermState) {
    ByteBuffersDirectory directory;
    FieldInfo field = make_field(IndexOptions::DocsAndFreqs);
    auto infos = field_infos_with(field);
    auto segment = segment_info_for();

    // A single-doc term is pulsed, so `decode_term` must read the singleton doc
    // id right after the doc start pointer.
    FieldWriteOptions options;
    options.write_freqs = true;
    SegmentWriteState state{get_default_info_stream(), directory, infos, io_context_default(), {}};
    auto writer = Lucene101PostingsWriter::create(state, segment);
    ASSERT_TRUE(writer.has_value());
    Lucene101PostingsWriter &postings = *writer.value();
    postings.set_field(field);
    ASSERT_TRUE(postings.start_term(options).has_value());
    ASSERT_TRUE(postings.start_doc(nullptr, 0, 1, options).has_value());
    ASSERT_TRUE(postings.finish_doc().has_value());
    auto term_state = postings.new_term_state();
    ASSERT_TRUE(term_state.has_value());
    auto block = term_state.value().get_block_term_state_mut();
    ASSERT_TRUE(block.has_value());
    (*block)->doc_freq = 1;
    (*block)->total_term_freq = 1;
    ASSERT_TRUE(postings.finish_term(term_state.value(), options).has_value());
    GrowableByteArrayDataOutput meta;
    ASSERT_TRUE(postings.encode_term_with_option(meta, field, term_state.value(), true, options).has_value());
    ASSERT_TRUE(postings.close().has_value());

    SegmentReadState read_state{directory, infos, io_context_default()};
    auto reader = Lucene101PostingsReader::create(read_state, segment);
    ASSERT_TRUE(reader.has_value());

    auto decoded_state = reader.value()->new_term_state();
    ASSERT_TRUE(decoded_state.has_value());
    auto decoded_block = decoded_state.value().get_block_term_state_mut();
    ASSERT_TRUE(decoded_block.has_value());
    (*decoded_block)->doc_freq = 1;
    (*decoded_block)->total_term_freq = 1;

    ByteArrayDataInput meta_in{meta.bytes()};
    ASSERT_TRUE(reader.value()->decode_term(meta_in, field, decoded_state.value(), true).has_value());
    const auto *decoded = std::get_if<IntBlockTermState>(&decoded_state.value().value());
    ASSERT_NE(decoded, nullptr);
    EXPECT_EQ(decoded->singleton_doc_id, 0);
}

TEST(Lucene101PostingsReaderTest, TermStateVariantMustBeInt) {
    ByteBuffersDirectory directory;
    FieldInfo field = make_field(IndexOptions::DocsAndFreqs);
    auto infos = field_infos_with(field);
    auto segment = segment_info_for();

    FieldWriteOptions options;
    options.write_freqs = true;
    ASSERT_TRUE(write_term(directory, infos, segment, field, options).has_value());

    SegmentReadState read_state{directory, infos, io_context_default()};
    auto reader = Lucene101PostingsReader::create(read_state, segment);
    ASSERT_TRUE(reader.has_value());

    TermStateEnum wrong{BlockTermState{}};
    std::vector<uint8_t> empty;
    ByteArrayDataInput meta_in{empty};
    auto decoded = reader.value()->decode_term(meta_in, field, wrong, true);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().kind(), ErrorKind::kIllegalState);

    EXPECT_EQ(reader.value()->new_term_state().value().value().index(), 1u);
    auto postings = reader.value()->postings(field, wrong, nullptr, FREQS);
    ASSERT_FALSE(postings.has_value());
    EXPECT_EQ(postings.error().kind(), ErrorKind::kIllegalState);
}

namespace {

/// Decodes the metadata of a term written by `write_term`.  The doc freq is
/// normally carried by the terms dictionary, so it has to be preset here as
/// well.
TermStateEnum decode_written_term(Lucene101PostingsReader &reader, const FieldInfo &field,
                                  const WrittenTerm &written) {
    TermStateEnum state = reader.new_term_state().value();
    auto block = state.get_block_term_state_mut();
    EXPECT_TRUE(block.has_value());
    (*block)->doc_freq = written.doc_freq;
    (*block)->total_term_freq = written.total_term_freq;
    ByteArrayDataInput meta_in{written.encoded};
    EXPECT_TRUE(reader.decode_term(meta_in, field, state, true).has_value());
    return state;
}

/// Writes one term over the docs 0 (three positions) and 4 (one position), with
/// offsets and one payload on the second position of doc 0.
Result<WrittenTerm> write_term_with_proximity(Directory &directory, std::shared_ptr<const FieldInfos> infos,
                                             const SegmentInfo &segment, const FieldInfo &field,
                                             const FieldWriteOptions &options) {
    SegmentWriteState state{get_default_info_stream(), directory, std::move(infos), io_context_default(), {}};
    auto writer = Lucene101PostingsWriter::create(state, segment);
    if (!writer) {
        return err(writer.error());
    }
    Lucene101PostingsWriter &postings = *writer.value();
    postings.set_field(field);
    if (auto started = postings.start_term(options); !started) {
        return err(started.error());
    }

    if (auto started = postings.start_doc(nullptr, 0, 3, options); !started) {
        return err(started.error());
    }
    if (auto added = postings.add_position(0, nullptr, 0, 3, options); !added) {
        return err(added.error());
    }
    const BytesRef payload{std::vector<uint8_t>{0xAA, 0xBB}};
    if (auto added = postings.add_position(2, options.write_payloads ? &payload : nullptr, 5, 9, options); !added) {
        return err(added.error());
    }
    if (auto added = postings.add_position(5, nullptr, 12, 20, options); !added) {
        return err(added.error());
    }
    if (auto finished = postings.finish_doc(); !finished) {
        return err(finished.error());
    }

    if (auto started = postings.start_doc(nullptr, 4, 1, options); !started) {
        return err(started.error());
    }
    if (auto added = postings.add_position(7, nullptr, 30, 33, options); !added) {
        return err(added.error());
    }
    if (auto finished = postings.finish_doc(); !finished) {
        return err(finished.error());
    }

    auto term_state = postings.new_term_state();
    if (!term_state) {
        return err(term_state.error());
    }
    auto block = term_state.value().get_block_term_state_mut();
    if (!block) {
        return err(block.error());
    }
    (*block)->doc_freq = 2;
    (*block)->total_term_freq = 4;
    if (auto finished = postings.finish_term(term_state.value(), options); !finished) {
        return err(finished.error());
    }

    GrowableByteArrayDataOutput meta;
    if (auto encoded = postings.encode_term_with_option(meta, field, term_state.value(), true, options);
        !encoded) {
        return err(encoded.error());
    }
    WrittenTerm written;
    written.encoded = meta.take_bytes();
    written.doc_freq = 2;
    written.total_term_freq = 4;
    if (auto closed = postings.close(); !closed) {
        return err(closed.error());
    }
    return written;
}

} // namespace

TEST(Lucene101PostingsReaderTest, EnumeratesDocsAndFreqs) {
    ByteBuffersDirectory directory;
    FieldInfo field = make_field(IndexOptions::DocsAndFreqs);
    auto infos = field_infos_with(field);
    auto segment = segment_info_for();

    FieldWriteOptions options;
    options.write_freqs = true;
    auto written = write_term(directory, infos, segment, field, options);
    ASSERT_TRUE(written.has_value());

    SegmentReadState read_state{directory, infos, io_context_default()};
    auto reader = Lucene101PostingsReader::create(read_state, segment);
    ASSERT_TRUE(reader.has_value());

    TermStateEnum state = decode_written_term(*reader.value(), field, written.value());
    auto postings = reader.value()->postings(field, state, nullptr, FREQS);
    ASSERT_TRUE(postings.has_value());
    PostingsEnum &docs = *postings.value();

    EXPECT_EQ(docs.cost().value(), 2);
    EXPECT_EQ(docs.doc_id(), -1);

    auto first = docs.next_doc();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first.value(), 0);
    EXPECT_EQ(docs.doc_id(), 0);
    EXPECT_EQ(docs.freq().value(), 2);

    auto second = docs.next_doc();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second.value(), 3);
    EXPECT_EQ(docs.freq().value(), 1);

    auto done = docs.next_doc();
    ASSERT_TRUE(done.has_value());
    EXPECT_EQ(done.value(), NO_MORE_DOCS);
}

TEST(Lucene101PostingsReaderTest, AdvanceSkipsToTargetDoc) {
    ByteBuffersDirectory directory;
    FieldInfo field = make_field(IndexOptions::DocsAndFreqs);
    auto infos = field_infos_with(field);
    auto segment = segment_info_for();

    FieldWriteOptions options;
    options.write_freqs = true;
    auto written = write_term(directory, infos, segment, field, options);
    ASSERT_TRUE(written.has_value());

    SegmentReadState read_state{directory, infos, io_context_default()};
    auto reader = Lucene101PostingsReader::create(read_state, segment);
    ASSERT_TRUE(reader.has_value());

    TermStateEnum state = decode_written_term(*reader.value(), field, written.value());
    auto postings = reader.value()->postings(field, state, nullptr, FREQS);
    ASSERT_TRUE(postings.has_value());
    PostingsEnum &docs = *postings.value();

    auto advanced = docs.advance(1);
    ASSERT_TRUE(advanced.has_value());
    EXPECT_EQ(advanced.value(), 3);
    EXPECT_EQ(docs.freq().value(), 1);

    auto exhausted = docs.advance(4);
    ASSERT_TRUE(exhausted.has_value());
    EXPECT_EQ(exhausted.value(), NO_MORE_DOCS);
}

TEST(Lucene101PostingsReaderTest, PostingsReuseResetsTheExistingEnum) {
    ByteBuffersDirectory directory;
    FieldInfo field = make_field(IndexOptions::DocsAndFreqs);
    auto infos = field_infos_with(field);
    auto segment = segment_info_for();

    FieldWriteOptions options;
    options.write_freqs = true;
    auto written = write_term(directory, infos, segment, field, options);
    ASSERT_TRUE(written.has_value());

    SegmentReadState read_state{directory, infos, io_context_default()};
    auto reader = Lucene101PostingsReader::create(read_state, segment);
    ASSERT_TRUE(reader.has_value());

    TermStateEnum state = decode_written_term(*reader.value(), field, written.value());
    auto postings = reader.value()->postings(field, state, nullptr, FREQS);
    ASSERT_TRUE(postings.has_value());
    ASSERT_TRUE(postings.value()->next_doc().has_value());
    PostingsEnum *original = postings.value().get();

    auto reused = reader.value()->postings(field, state, std::move(postings.value()), FREQS);
    ASSERT_TRUE(reused.has_value());
    EXPECT_EQ(reused.value().get(), original);

    auto first = reused.value()->next_doc();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first.value(), 0);
    EXPECT_EQ(reused.value()->freq().value(), 2);
}

TEST(Lucene101PostingsReaderTest, EnumeratesPositionsOffsetsAndPayloads) {
    ByteBuffersDirectory directory;
    FieldInfo field = make_field(IndexOptions::DocsAndFreqsAndPositionsAndOffsets, true);
    auto infos = field_infos_with(field);
    auto segment = segment_info_for();

    FieldWriteOptions options;
    options.write_freqs = true;
    options.write_positions = true;
    options.write_offsets = true;
    options.write_payloads = true;
    auto written = write_term_with_proximity(directory, infos, segment, field, options);
    ASSERT_TRUE(written.has_value());

    SegmentReadState read_state{directory, infos, io_context_default()};
    auto reader = Lucene101PostingsReader::create(read_state, segment);
    ASSERT_TRUE(reader.has_value());
    EXPECT_TRUE(reader.value()->has_positions());
    EXPECT_TRUE(reader.value()->has_payloads());

    TermStateEnum state = decode_written_term(*reader.value(), field, written.value());
    auto postings = reader.value()->postings(field, state, nullptr, OFFSETS | PAYLOADS);
    ASSERT_TRUE(postings.has_value());
    PostingsEnum &docs = *postings.value();

    ASSERT_EQ(docs.next_doc().value(), 0);
    EXPECT_EQ(docs.freq().value(), 3);

    EXPECT_EQ(docs.next_position().value(), 0);
    EXPECT_EQ(docs.start_offset().value(), 0);
    EXPECT_EQ(docs.end_offset().value(), 3);
    EXPECT_FALSE(docs.get_payload().value().has_value());

    EXPECT_EQ(docs.next_position().value(), 2);
    EXPECT_EQ(docs.start_offset().value(), 5);
    EXPECT_EQ(docs.end_offset().value(), 9);
    auto payload = docs.get_payload();
    ASSERT_TRUE(payload.has_value());
    ASSERT_TRUE(payload.value().has_value());
    ASSERT_EQ(payload.value().value().size(), 2u);
    EXPECT_EQ(payload.value().value()[0], 0xAA);
    EXPECT_EQ(payload.value().value()[1], 0xBB);

    EXPECT_EQ(docs.next_position().value(), 5);
    EXPECT_EQ(docs.start_offset().value(), 12);
    EXPECT_EQ(docs.end_offset().value(), 20);

    ASSERT_EQ(docs.next_doc().value(), 4);
    EXPECT_EQ(docs.freq().value(), 1);
    EXPECT_EQ(docs.next_position().value(), 7);
    EXPECT_EQ(docs.start_offset().value(), 30);
    EXPECT_EQ(docs.end_offset().value(), 33);

    EXPECT_EQ(docs.next_doc().value(), NO_MORE_DOCS);
}

TEST(Lucene101PostingsReaderTest, MultiBlockTermWalksAndAdvancesAcrossBlocks) {
    constexpr int32_t kDocs = 300;
    ByteBuffersDirectory directory;
    FieldInfo field = make_field(IndexOptions::DocsAndFreqs);
    auto infos = field_infos_with(field);
    auto segment = segment_info_for();

    FieldWriteOptions options;
    options.write_freqs = true;

    SegmentWriteState write_state{get_default_info_stream(), directory, infos, io_context_default(), {}};
    auto writer = Lucene101PostingsWriter::create(write_state, segment);
    ASSERT_TRUE(writer.has_value());
    Lucene101PostingsWriter &postings_writer = *writer.value();
    postings_writer.set_field(field);
    ASSERT_TRUE(postings_writer.start_term(options).has_value());

    int64_t total_term_freq = 0;
    for (int32_t i = 0; i < kDocs; ++i) {
        const int32_t freq = i % 7 + 1;
        total_term_freq += freq;
        ASSERT_TRUE(postings_writer.start_doc(nullptr, i * 3, freq, options).has_value());
        ASSERT_TRUE(postings_writer.finish_doc().has_value());
    }

    auto term_state = postings_writer.new_term_state();
    ASSERT_TRUE(term_state.has_value());
    auto block = term_state.value().get_block_term_state_mut();
    ASSERT_TRUE(block.has_value());
    (*block)->doc_freq = kDocs;
    (*block)->total_term_freq = total_term_freq;
    ASSERT_TRUE(postings_writer.finish_term(term_state.value(), options).has_value());
    GrowableByteArrayDataOutput meta;
    ASSERT_TRUE(
            postings_writer.encode_term_with_option(meta, field, term_state.value(), true, options).has_value());
    ASSERT_TRUE(postings_writer.close().has_value());

    WrittenTerm written;
    written.encoded = meta.take_bytes();
    written.doc_freq = kDocs;
    written.total_term_freq = total_term_freq;

    SegmentReadState read_state{directory, infos, io_context_default()};
    auto reader = Lucene101PostingsReader::create(read_state, segment);
    ASSERT_TRUE(reader.has_value());
    TermStateEnum state = decode_written_term(*reader.value(), field, written);

    auto walk = reader.value()->postings(field, state, nullptr, FREQS);
    ASSERT_TRUE(walk.has_value());
    EXPECT_EQ(walk.value()->cost().value(), kDocs);
    for (int32_t i = 0; i < kDocs; ++i) {
        auto doc = walk.value()->next_doc();
        ASSERT_TRUE(doc.has_value());
        ASSERT_EQ(doc.value(), i * 3);
        ASSERT_EQ(walk.value()->freq().value(), i % 7 + 1);
    }
    EXPECT_EQ(walk.value()->next_doc().value(), NO_MORE_DOCS);

    auto jumping = reader.value()->postings(field, state, nullptr, FREQS);
    ASSERT_TRUE(jumping.has_value());
    auto advanced = jumping.value()->advance(600);
    ASSERT_TRUE(advanced.has_value());
    EXPECT_EQ(advanced.value(), 600);
    EXPECT_EQ(jumping.value()->freq().value(), 200 % 7 + 1);

    auto skipped = jumping.value()->advance(601);
    ASSERT_TRUE(skipped.has_value());
    EXPECT_EQ(skipped.value(), 603);

    auto tail = jumping.value()->advance(897);
    ASSERT_TRUE(tail.has_value());
    EXPECT_EQ(tail.value(), 897);
    EXPECT_EQ(jumping.value()->next_doc().value(), NO_MORE_DOCS);
}

TEST(Lucene101PostingsReaderTest, LargeTermCrossesTheLevel1SkipBoundary) {
    // More than LEVEL1_NUM_DOCS docs, so the term has two level 1 blocks.
    constexpr int32_t kDocs = 1100;
    ByteBuffersDirectory directory;
    FieldInfo field = make_field(IndexOptions::DocsAndFreqs);
    auto infos = field_infos_with(field);
    auto segment = segment_info_for();

    FieldWriteOptions options;
    options.write_freqs = true;

    SegmentWriteState write_state{get_default_info_stream(), directory, infos, io_context_default(), {}};
    auto writer = Lucene101PostingsWriter::create(write_state, segment);
    ASSERT_TRUE(writer.has_value());
    Lucene101PostingsWriter &postings_writer = *writer.value();
    postings_writer.set_field(field);
    ASSERT_TRUE(postings_writer.start_term(options).has_value());

    for (int32_t i = 0; i < kDocs; ++i) {
        ASSERT_TRUE(postings_writer.start_doc(nullptr, i * 2, 1, options).has_value());
        ASSERT_TRUE(postings_writer.finish_doc().has_value());
    }

    auto term_state = postings_writer.new_term_state();
    ASSERT_TRUE(term_state.has_value());
    auto block = term_state.value().get_block_term_state_mut();
    ASSERT_TRUE(block.has_value());
    (*block)->doc_freq = kDocs;
    (*block)->total_term_freq = kDocs;
    ASSERT_TRUE(postings_writer.finish_term(term_state.value(), options).has_value());
    GrowableByteArrayDataOutput meta;
    ASSERT_TRUE(
            postings_writer.encode_term_with_option(meta, field, term_state.value(), true, options).has_value());
    ASSERT_TRUE(postings_writer.close().has_value());

    WrittenTerm written;
    written.encoded = meta.take_bytes();
    written.doc_freq = kDocs;
    written.total_term_freq = kDocs;

    SegmentReadState read_state{directory, infos, io_context_default()};
    auto reader = Lucene101PostingsReader::create(read_state, segment);
    ASSERT_TRUE(reader.has_value());
    TermStateEnum state = decode_written_term(*reader.value(), field, written);

    auto postings = reader.value()->postings(field, state, nullptr, FREQS);
    ASSERT_TRUE(postings.has_value());

    // Jump straight into the second level 1 block.
    auto advanced = postings.value()->advance(2060);
    ASSERT_TRUE(advanced.has_value());
    EXPECT_EQ(advanced.value(), 2060);
    EXPECT_EQ(postings.value()->freq().value(), 1);

    auto next = postings.value()->next_doc();
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next.value(), 2062);

    auto last = postings.value()->advance(kDocs * 2);
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last.value(), NO_MORE_DOCS);
}

TEST(Lucene101PostingsReaderTest, ImpactsEnumExposesTheCurrentLevel0Bound) {
    ByteBuffersDirectory directory;
    FieldInfo field = make_field(IndexOptions::DocsAndFreqs);
    auto infos = field_infos_with(field);
    auto segment = segment_info_for();

    FieldWriteOptions options;
    options.write_freqs = true;
    auto written = write_term(directory, infos, segment, field, options);
    ASSERT_TRUE(written.has_value());

    SegmentReadState read_state{directory, infos, io_context_default()};
    auto reader = Lucene101PostingsReader::create(read_state, segment);
    ASSERT_TRUE(reader.has_value());

    TermStateEnum state = decode_written_term(*reader.value(), field, written.value());
    auto impacts = reader.value()->impacts(field, state, FREQS);
    ASSERT_TRUE(impacts.has_value());
    EXPECT_EQ(impacts.value()->next_doc().value(), 0);

    auto source = impacts.value()->get_impacts();
    ASSERT_TRUE(source.has_value());
    EXPECT_EQ(source.value()->num_levels(), 1u);
    EXPECT_EQ(source.value()->get_doc_id_upto(0), NO_MORE_DOCS);
}
