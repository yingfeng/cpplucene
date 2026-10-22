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

module lucene_core:ut.lucene90_compressing_term_vectors;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view FORMAT_NAME = "Lucene90TermVectorsTest";

/// The three shapes of vector a test document can carry.
enum FieldSlot : int32_t {
    /// positions + offsets + payloads
    FULL = 0,
    /// positions only
    POSITIONS_ONLY = 1,
    /// frequencies only
    FREQ_ONLY = 2,
    COUNT = 3,
};

/// One term of a vector.
struct TermSpec {
    std::string term;
    int32_t freq{1};
    std::vector<int32_t> positions;
    std::vector<int32_t> start_offsets;
    std::vector<int32_t> lengths;
    /// One entry per occurrence; an empty vector means "no payload".
    std::vector<std::vector<uint8_t>> payloads;
};

std::span<const uint8_t> bytes_of(const std::string &text) {
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

std::span<const uint8_t> bytes_of(const std::vector<uint8_t> &bytes) {
    return std::span<const uint8_t>(bytes);
}

std::string text_of(const BytesRef &term) {
    return std::string(reinterpret_cast<const char *>(term.bytes().data() + term.offset()), term.length());
}

std::string text_of(std::span<const uint8_t> bytes) {
    return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

struct Fixture {
    Fixture() : infos(build_infos()) {}

    ByteBuffersDirectory directory;
    std::shared_ptr<const FieldInfos> infos;

    static std::shared_ptr<const FieldInfos> build_infos() {
        std::vector<FieldInfo> fields;
        const char *names[COUNT] = {"full", "positions", "freq"};
        for (int32_t number = 0; number < COUNT; ++number) {
            FieldInfo field;
            field.number = number;
            field.name = names[number];
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
        info.id.assign(ID_LENGTH, 0x33);
        info.max_doc = max_doc;
        return info;
    }

    [[nodiscard]] std::unique_ptr<TermVectorsWriter> writer(int32_t chunk_size, int32_t max_docs_per_chunk,
                                                           int32_t num_docs) {
        auto format = Lucene90CompressingTermVectorsFormat::create(std::string(FORMAT_NAME), "",
                                                                   CompressionMode::fast(), chunk_size,
                                                                   max_docs_per_chunk, /*block_size=*/4);
        EXPECT_TRUE(format.has_value()) << (format ? "" : format.error().to_string());
        if (!format.has_value()) {
            return nullptr;
        }
        SegmentInfo info = segment(num_docs);
        auto writer = format.value().vectors_writer(directory, info, io_context_default());
        EXPECT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
        return writer.has_value() ? std::move(writer.value()) : nullptr;
    }

    [[nodiscard]] std::unique_ptr<Lucene90CompressingTermVectorsReader> reader(int32_t num_docs) {
        // The reader ignores the writer-only parameters, but `create` validates
        // them, so a valid configuration is needed.
        auto format = Lucene90CompressingTermVectorsFormat::create(std::string(FORMAT_NAME), "",
                                                                  CompressionMode::fast(), 4096, 128, 4);
        EXPECT_TRUE(format.has_value());
        SegmentInfo info = segment(num_docs);
        auto reader = format.value().vectors_reader(directory, info, infos, io_context_default());
        EXPECT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
        if (!reader.has_value()) {
            return nullptr;
        }
        auto *concrete = dynamic_cast<Lucene90CompressingTermVectorsReader *>(reader.value().release());
        EXPECT_TRUE(concrete != nullptr);
        return std::unique_ptr<Lucene90CompressingTermVectorsReader>(concrete);
    }

    /// Writes `fields` as one document: each entry is the field number and its
    /// vector.
    void write_vector(TermVectorsWriter &writer, const std::vector<std::pair<int32_t, std::vector<TermSpec>>> &fields) {
        ASSERT_TRUE(writer.start_document(static_cast<int32_t>(fields.size())).has_value());
        for (const auto &[field_number, terms] : fields) {
            const bool full = field_number == FULL;
            const bool positions = full || field_number == POSITIONS_ONLY;
            const bool offsets = full;
            const bool payloads = full;
            ASSERT_TRUE(writer.start_field(field(field_number), static_cast<int32_t>(terms.size()), positions,
                                           offsets, payloads)
                                .has_value());
            for (const TermSpec &term : terms) {
                ASSERT_TRUE(writer.start_term(bytes_of(term.term), term.freq).has_value());
                for (int32_t occurrence = 0; occurrence < term.freq; ++occurrence) {
                    std::optional<std::span<const uint8_t>> payload;
                    if (payloads && static_cast<size_t>(occurrence) < term.payloads.size() &&
                        !term.payloads[static_cast<size_t>(occurrence)].empty()) {
                        payload = bytes_of(term.payloads[static_cast<size_t>(occurrence)]);
                    }
                    const int32_t position = term.positions.empty() ? occurrence
                                                                    : term.positions[static_cast<size_t>(occurrence)];
                    const int32_t start = term.start_offsets.empty() ? position
                                                                     : term.start_offsets[static_cast<size_t>(occurrence)];
                    const int32_t length = term.lengths.empty() ? 1 : term.lengths[static_cast<size_t>(occurrence)];
                    ASSERT_TRUE(writer.add_position(position, start, start + length, payload).has_value());
                }
            }
            ASSERT_TRUE(writer.finish_field().has_value());
        }
        ASSERT_TRUE(writer.finish_document().has_value());
    }

    /// The terms of `field` of the document `doc`, as `(term, freq)` pairs.
    [[nodiscard]] static std::vector<std::pair<std::string, int32_t>> terms_of(TermVectorsReader &reader, int32_t doc,
                                                                              std::string_view field_name) {
        std::vector<std::pair<std::string, int32_t>> result;
        auto vectors = reader.get(doc);
        EXPECT_TRUE(vectors.has_value()) << (vectors ? "" : vectors.error().to_string());
        if (!vectors.has_value() || vectors.value() == nullptr) {
            return result;
        }
        auto terms = vectors.value()->terms(field_name);
        EXPECT_TRUE(terms.has_value()) << (terms ? "" : terms.error().to_string());
        if (!terms.has_value() || terms.value() == nullptr) {
            return result;
        }
        auto iterator = terms.value()->iterator();
        EXPECT_TRUE(iterator.has_value());
        if (!iterator.has_value()) {
            return result;
        }
        for (;;) {
            auto term = iterator.value()->next();
            EXPECT_TRUE(term.has_value()) << (term ? "" : term.error().to_string());
            if (!term.has_value() || term.value() == nullptr) {
                break;
            }
            auto freq = iterator.value()->doc_freq();
            EXPECT_TRUE(freq.has_value());
            result.emplace_back(text_of(*term.value()), freq.has_value() ? freq.value() : 0);
        }
        return result;
    }
};

std::vector<TermSpec> apple_terms() {
    TermSpec apple;
    apple.term = "apple";
    apple.freq = 2;
    apple.positions = {0, 5};
    apple.start_offsets = {0, 6};
    apple.lengths = {5, 5};
    apple.payloads = {{'p', '1'}, {'p', '2', '2'}};
    return {apple};
}

} // namespace

TEST(Lucene90CompressingTermVectorsTest, WritesAndReadsBackTermVectors) {
    Fixture fixture;
    auto writer = fixture.writer(/*chunk_size=*/4096, /*max_docs_per_chunk=*/128, /*num_docs=*/1);
    ASSERT_TRUE(writer != nullptr);

    std::vector<TermSpec> freq_terms;
    {
        TermSpec beta;
        beta.term = "beta";
        beta.freq = 3;
        freq_terms.push_back(std::move(beta));
    }
    fixture.write_vector(*writer, {{FULL, apple_terms()}, {FREQ_ONLY, freq_terms}});
    ASSERT_TRUE(writer->finish(1).has_value());
    ASSERT_TRUE(writer->close().has_value());

    auto reader = fixture.reader(1);
    ASSERT_TRUE(reader != nullptr);
    EXPECT_EQ(reader->num_docs(), 1);
    EXPECT_EQ(reader->chunk_size(), 4096);
    EXPECT_EQ(reader->num_chunks(), 1);
    EXPECT_EQ(reader->num_dirty_chunks(), 1);
    EXPECT_EQ(reader->num_dirty_docs(), 1);

    auto vectors = reader->get(0);
    ASSERT_TRUE(vectors.has_value()) << (vectors ? "" : vectors.error().to_string());
    ASSERT_TRUE(vectors.value() != nullptr);
    EXPECT_EQ(vectors.value()->size().value(), 2);

    // Field names come back in the order they were written.
    {
        std::vector<std::string> names;
        auto iterator = vectors.value()->iterator();
        ASSERT_TRUE(iterator.has_value());
        for (;;) {
            auto name = iterator.value()->next();
            ASSERT_TRUE(name.has_value());
            if (name.value() == nullptr) {
                break;
            }
            names.push_back(*name.value());
        }
        EXPECT_EQ(names, (std::vector<std::string>{"full", "freq"}));
    }

    auto terms = vectors.value()->terms("full");
    ASSERT_TRUE(terms.has_value());
    ASSERT_TRUE(terms.value() != nullptr);
    EXPECT_EQ(terms.value()->size().value(), 1);
    EXPECT_EQ(terms.value()->doc_count().value(), 1);
    EXPECT_EQ(terms.value()->sum_total_term_freq().value(), 2);
    EXPECT_EQ(terms.value()->sum_doc_freq().value(), 1);
    EXPECT_TRUE(terms.value()->has_freqs());
    EXPECT_TRUE(terms.value()->has_positions());
    EXPECT_TRUE(terms.value()->has_offsets());
    EXPECT_TRUE(terms.value()->has_payloads());

    auto iterator = terms.value()->iterator();
    ASSERT_TRUE(iterator.has_value());
    auto term = iterator.value()->next();
    ASSERT_TRUE(term.has_value());
    ASSERT_TRUE(term.value() != nullptr);
    EXPECT_EQ(text_of(*term.value()), "apple");
    EXPECT_EQ(iterator.value()->doc_freq().value(), 1);
    EXPECT_EQ(iterator.value()->total_term_freq().value(), 2);
    ASSERT_TRUE(iterator.value()->next().value() == nullptr);

    auto postings = iterator.value()->postings_with_flags(nullptr, 0);
    ASSERT_TRUE(postings.has_value()) << (postings ? "" : postings.error().to_string());
    EXPECT_EQ(postings.value()->next_doc().value(), 0);
    EXPECT_EQ(postings.value()->freq().value(), 2);

    EXPECT_EQ(postings.value()->next_position().value(), 0);
    EXPECT_EQ(postings.value()->start_offset().value(), 0);
    EXPECT_EQ(postings.value()->end_offset().value(), 5);
    auto payload = postings.value()->get_payload();
    ASSERT_TRUE(payload.has_value());
    ASSERT_TRUE(payload.value().has_value());
    EXPECT_EQ(text_of(*payload.value()), "p1");

    EXPECT_EQ(postings.value()->next_position().value(), 5);
    EXPECT_EQ(postings.value()->start_offset().value(), 6);
    EXPECT_EQ(postings.value()->end_offset().value(), 11);
    payload = postings.value()->get_payload();
    ASSERT_TRUE(payload.has_value());
    ASSERT_TRUE(payload.value().has_value());
    EXPECT_EQ(text_of(*payload.value()), "p22");

    // Beyond the last position.
    EXPECT_FALSE(postings.value()->next_position().has_value());
    EXPECT_EQ(postings.value()->next_doc().value(), NO_MORE_DOCS);

    // The frequencies-only field has no positions, offsets or payloads.
    auto freq_vectors = vectors.value()->terms("freq");
    ASSERT_TRUE(freq_vectors.has_value());
    ASSERT_TRUE(freq_vectors.value() != nullptr);
    EXPECT_FALSE(freq_vectors.value()->has_positions());
    EXPECT_FALSE(freq_vectors.value()->has_offsets());
    EXPECT_FALSE(freq_vectors.value()->has_payloads());
    EXPECT_EQ(freq_vectors.value()->sum_total_term_freq().value(), 3);

    auto freq_iterator = freq_vectors.value()->iterator();
    ASSERT_TRUE(freq_iterator.has_value());
    auto beta = freq_iterator.value()->next();
    ASSERT_TRUE(beta.has_value());
    ASSERT_TRUE(beta.value() != nullptr);
    EXPECT_EQ(text_of(*beta.value()), "beta");
    auto beta_postings = freq_iterator.value()->postings_with_flags(nullptr, 0);
    ASSERT_TRUE(beta_postings.has_value());
    EXPECT_EQ(beta_postings.value()->next_doc().value(), 0);
    EXPECT_EQ(beta_postings.value()->freq().value(), 3);
    EXPECT_EQ(beta_postings.value()->next_position().value(), -1);
    EXPECT_EQ(beta_postings.value()->start_offset().value(), -1);
    EXPECT_EQ(beta_postings.value()->end_offset().value(), -1);
    auto no_payload = beta_postings.value()->get_payload();
    ASSERT_TRUE(no_payload.has_value());
    EXPECT_FALSE(no_payload.value().has_value());

    EXPECT_TRUE(reader->check_integrity().has_value());
}

TEST(Lucene90CompressingTermVectorsTest, PrefixCompressionRoundTrips) {
    Fixture fixture;
    auto writer = fixture.writer(4096, 128, 1);
    ASSERT_TRUE(writer != nullptr);

    // Terms must be written in lexicographic order: that is what makes prefix
    // compression and seek work.
    std::vector<TermSpec> terms;
    for (const char *text : {"abc", "abcd", "abd", "b", "zzz"}) {
        TermSpec spec;
        spec.term = text;
        spec.freq = 1;
        terms.push_back(std::move(spec));
    }
    fixture.write_vector(*writer, {{FREQ_ONLY, terms}});
    ASSERT_TRUE(writer->finish(1).has_value());
    ASSERT_TRUE(writer->close().has_value());

    auto reader = fixture.reader(1);
    ASSERT_TRUE(reader != nullptr);
    const auto read = Fixture::terms_of(*reader, 0, "freq");
    ASSERT_EQ(read.size(), 5);
    EXPECT_EQ(read[0].first, "abc");
    EXPECT_EQ(read[1].first, "abcd");
    EXPECT_EQ(read[2].first, "abd");
    EXPECT_EQ(read[3].first, "b");
    EXPECT_EQ(read[4].first, "zzz");
    for (const auto &entry : read) {
        EXPECT_EQ(entry.second, 1);
    }

    // Seeking reuses the prefix compression state.
    auto vectors = reader->get(0);
    ASSERT_TRUE(vectors.has_value());
    auto field_terms = vectors.value()->terms("freq");
    ASSERT_TRUE(field_terms.has_value());
    auto iterator = field_terms.value()->iterator();
    ASSERT_TRUE(iterator.has_value());
    EXPECT_EQ(iterator.value()->seek_exact(bytes_of(std::string("abcd"))).value(), true);
    EXPECT_EQ(text_of(*iterator.value()->term().value()), "abcd");
    EXPECT_EQ(iterator.value()->seek_ceil(bytes_of(std::string("abce"))).value(), SeekStatus::NotFound);
    EXPECT_EQ(text_of(*iterator.value()->term().value()), "abd");
    EXPECT_EQ(iterator.value()->seek_exact(bytes_of(std::string("nope"))).value(), false);
    EXPECT_EQ(iterator.value()->seek_ceil(bytes_of(std::string("zzzz"))).value(), SeekStatus::End);
}

TEST(Lucene90CompressingTermVectorsTest, MultipleDocumentsAndChunks) {
    Fixture fixture;
    constexpr int32_t NUM_DOCS = 5;
    auto writer = fixture.writer(/*chunk_size=*/16, /*max_docs_per_chunk=*/128, NUM_DOCS);
    ASSERT_TRUE(writer != nullptr);

    for (int32_t doc = 0; doc < NUM_DOCS; ++doc) {
        std::vector<TermSpec> terms;
        TermSpec first;
        first.term = "term" + std::to_string(doc);
        first.freq = 1;
        first.positions = {doc};
        first.start_offsets = {doc * 10};
        first.lengths = {4};
        terms.push_back(std::move(first));
        if (doc % 2 == 0) {
            TermSpec second;
            second.term = "extra" + std::to_string(doc);
            second.freq = 2;
            second.positions = {doc, doc + 1};
            second.start_offsets = {doc * 10, doc * 10 + 5};
            second.lengths = {4, 4};
            terms.insert(terms.begin(), std::move(second));
        }
        fixture.write_vector(*writer, {{FULL, terms}});
    }
    ASSERT_TRUE(writer->finish(NUM_DOCS).has_value());
    ASSERT_TRUE(writer->close().has_value());

    auto reader = fixture.reader(NUM_DOCS);
    ASSERT_TRUE(reader != nullptr);
    EXPECT_EQ(reader->num_docs(), NUM_DOCS);
    EXPECT_GT(reader->num_chunks(), 1);

    // Read out of order: every document must be found in its own chunk.
    for (int32_t doc = NUM_DOCS - 1; doc >= 0; --doc) {
        const auto read = Fixture::terms_of(*reader, doc, "full");
        ASSERT_EQ(read.size(), doc % 2 == 0 ? 2 : 1);
        EXPECT_EQ(read.back().first, "term" + std::to_string(doc));
    }

    // And again in order, which reuses the decoded chunk.
    for (int32_t doc = 0; doc < NUM_DOCS; ++doc) {
        const auto read = Fixture::terms_of(*reader, doc, "full");
        EXPECT_EQ(read.size(), doc % 2 == 0 ? 2 : 1);
    }
    EXPECT_TRUE(reader->check_integrity().has_value());
}

TEST(Lucene90CompressingTermVectorsTest, DocumentsWithoutVectorsAndUnknownFields) {
    Fixture fixture;
    auto writer = fixture.writer(4096, 128, 3);
    ASSERT_TRUE(writer != nullptr);

    // doc 0: no vector at all
    ASSERT_TRUE(writer->start_document(0).has_value());
    ASSERT_TRUE(writer->finish_document().has_value());
    // doc 1: one field with one term
    fixture.write_vector(*writer, {{FREQ_ONLY, [] {
                                        TermSpec spec;
                                        spec.term = "only";
                                        spec.freq = 1;
                                        return std::vector<TermSpec>{spec};
                                    }()}});
    // doc 2: no vector at all
    ASSERT_TRUE(writer->start_document(0).has_value());
    ASSERT_TRUE(writer->finish_document().has_value());
    ASSERT_TRUE(writer->finish(3).has_value());
    ASSERT_TRUE(writer->close().has_value());

    auto reader = fixture.reader(3);
    ASSERT_TRUE(reader != nullptr);
    for (const int32_t doc : {0, 2}) {
        auto vectors = reader->get(doc);
        ASSERT_TRUE(vectors.has_value()) << (vectors ? "" : vectors.error().to_string());
        EXPECT_TRUE(vectors.value() == nullptr) << "doc " << doc << " has no term vectors";
    }

    auto vectors = reader->get(1);
    ASSERT_TRUE(vectors.has_value());
    ASSERT_TRUE(vectors.value() != nullptr);
    EXPECT_EQ(vectors.value()->size().value(), 1);

    // A field that has no vector in this document.
    auto missing = vectors.value()->terms("full");
    ASSERT_TRUE(missing.has_value());
    EXPECT_TRUE(missing.value() == nullptr);

    // A field that is not in the segment at all.
    auto unknown = vectors.value()->terms("no-such-field");
    ASSERT_TRUE(unknown.has_value());
    EXPECT_TRUE(unknown.value() == nullptr);

    // get_field_terms goes through get().
    auto direct = reader->get_field_terms(1, "freq");
    ASSERT_TRUE(direct.has_value());
    ASSERT_TRUE(direct.value() != nullptr);
    EXPECT_EQ(direct.value()->size().value(), 1);

    auto none = reader->get_field_terms(1, "full");
    ASSERT_TRUE(none.has_value());
    EXPECT_TRUE(none.value() == nullptr);

    auto no_doc = reader->get_field_terms(0, "freq");
    ASSERT_TRUE(no_doc.has_value());
    EXPECT_TRUE(no_doc.value() == nullptr);
}

TEST(Lucene90CompressingTermVectorsTest, PerFieldFlagsMayVaryWithinAChunk) {
    Fixture fixture;
    auto writer = fixture.writer(/*chunk_size=*/1 << 16, /*max_docs_per_chunk=*/8, /*num_docs=*/2);
    ASSERT_TRUE(writer != nullptr);

    // doc 0 stores payloads for "full", doc 1 does not: the chunk cannot use one
    // flag per field, it has to store one flag per field instance.
    std::vector<TermSpec> with_payload = apple_terms();
    fixture.write_vector(*writer, {{FULL, with_payload}});
    std::vector<TermSpec> without_payload = apple_terms();
    without_payload[0].payloads.clear();
    fixture.write_vector(*writer, {{FULL, without_payload}});
    ASSERT_TRUE(writer->finish(2).has_value());
    ASSERT_TRUE(writer->close().has_value());

    auto reader = fixture.reader(2);
    ASSERT_TRUE(reader != nullptr);
    EXPECT_EQ(reader->num_chunks(), 1);
    EXPECT_EQ(reader->num_dirty_chunks(), 1);

    for (int32_t doc = 0; doc < 2; ++doc) {
        auto terms = reader->get_field_terms(doc, "full");
        ASSERT_TRUE(terms.has_value()) << (terms ? "" : terms.error().to_string());
        ASSERT_TRUE(terms.value() != nullptr);
        EXPECT_TRUE(terms.value()->has_payloads());
        auto iterator = terms.value()->iterator();
        ASSERT_TRUE(iterator.has_value());
        auto term = iterator.value()->next();
        ASSERT_TRUE(term.has_value());
        ASSERT_TRUE(term.value() != nullptr);
        EXPECT_EQ(text_of(*term.value()), "apple");
        auto postings = iterator.value()->postings_with_flags(nullptr, 0);
        ASSERT_TRUE(postings.has_value());
        EXPECT_EQ(postings.value()->next_doc().value(), 0);
        EXPECT_EQ(postings.value()->next_position().value(), 0);
        auto payload = postings.value()->get_payload();
        ASSERT_TRUE(payload.has_value());
        if (doc == 0) {
            ASSERT_TRUE(payload.value().has_value());
            EXPECT_EQ(text_of(*payload.value()), "p1");
        } else {
            EXPECT_FALSE(payload.value().has_value());
        }
    }
}

TEST(Lucene90CompressingTermVectorsTest, CloneAndMergeInstanceAreIndependent) {
    Fixture fixture;
    auto writer = fixture.writer(4096, 128, 2);
    ASSERT_TRUE(writer != nullptr);
    for (int32_t doc = 0; doc < 2; ++doc) {
        std::vector<TermSpec> terms;
        TermSpec spec;
        spec.term = "doc" + std::to_string(doc);
        spec.freq = 1;
        terms.push_back(std::move(spec));
        fixture.write_vector(*writer, {{FREQ_ONLY, terms}});
    }
    ASSERT_TRUE(writer->finish(2).has_value());
    ASSERT_TRUE(writer->close().has_value());

    auto reader = fixture.reader(2);
    ASSERT_TRUE(reader != nullptr);
    auto clone = reader->clone();
    ASSERT_TRUE(clone.has_value()) << (clone ? "" : clone.error().to_string());
    auto merge_instance = reader->get_merge_instance();
    ASSERT_TRUE(merge_instance.has_value()) << (merge_instance ? "" : merge_instance.error().to_string());

    // Interleaved reads: each reader keeps its own cursor.
    EXPECT_EQ(Fixture::terms_of(*reader, 0, "freq").at(0).first, "doc0");
    EXPECT_EQ(Fixture::terms_of(*clone.value(), 1, "freq").at(0).first, "doc1");
    EXPECT_EQ(Fixture::terms_of(*merge_instance.value(), 1, "freq").at(0).first, "doc1");
    EXPECT_EQ(Fixture::terms_of(*reader, 1, "freq").at(0).first, "doc1");

    // Closing the original does not close the copies.
    ASSERT_TRUE(reader->close().has_value());
    EXPECT_FALSE(reader->get(0).has_value());
    EXPECT_EQ(Fixture::terms_of(*clone.value(), 0, "freq").at(0).first, "doc0");
    ASSERT_TRUE(clone.value()->close().has_value());
    ASSERT_TRUE(merge_instance.value()->close().has_value());
}

TEST(Lucene90CompressingTermVectorsTest, AddProxReadsRawPositionStreams) {
    Fixture fixture;
    auto writer = fixture.writer(4096, 128, 1);
    ASSERT_TRUE(writer != nullptr);

    ASSERT_TRUE(writer->start_document(1).has_value());
    // One term with two occurrences.
    ASSERT_TRUE(writer->start_field(fixture.field(FULL), 1, true, true, true).has_value());

    // Position stream of the first term: code = (delta << 1) | hasPayload.
    //   occurrence 0: position 0, payload "ab"
    //   occurrence 1: position +3, no payload
    std::vector<uint8_t> position_data;
    {
        ByteArrayOutput out("positions");
        ASSERT_TRUE(out.write_vint(0 << 1 | 1).has_value());
        ASSERT_TRUE(out.write_vint(2).has_value());
        ASSERT_TRUE(out.write_bytes(bytes_of(std::string("ab")), 0, 2).has_value());
        ASSERT_TRUE(out.write_vint(3 << 1).has_value());
        position_data.assign(out.bytes().begin(), out.bytes().end());
    }
    // Offset stream: (start delta, length) per occurrence.
    std::vector<uint8_t> offset_data;
    {
        ByteArrayOutput out("offsets");
        ASSERT_TRUE(out.write_vint(5).has_value());
        ASSERT_TRUE(out.write_vint(2).has_value());
        ASSERT_TRUE(out.write_vint(5).has_value());
        ASSERT_TRUE(out.write_vint(4).has_value());
        offset_data.assign(out.bytes().begin(), out.bytes().end());
    }
    ByteArrayDataInput positions(position_data);
    ByteArrayDataInput offsets(offset_data);

    ASSERT_TRUE(writer->start_term(bytes_of(std::string("term")), 2).has_value());
    ASSERT_TRUE(writer->add_prox(2, &positions, &offsets).has_value());
    ASSERT_TRUE(writer->finish_field().has_value());
    ASSERT_TRUE(writer->finish_document().has_value());
    ASSERT_TRUE(writer->finish(1).has_value());
    ASSERT_TRUE(writer->close().has_value());

    auto reader = fixture.reader(1);
    ASSERT_TRUE(reader != nullptr);
    auto terms = reader->get_field_terms(0, "full");
    ASSERT_TRUE(terms.has_value());
    ASSERT_TRUE(terms.value() != nullptr);
    EXPECT_EQ(terms.value()->sum_total_term_freq().value(), 2);

    auto iterator = terms.value()->iterator();
    ASSERT_TRUE(iterator.has_value());
    auto term = iterator.value()->next();
    ASSERT_TRUE(term.has_value());
    ASSERT_TRUE(term.value() != nullptr);
    EXPECT_EQ(text_of(*term.value()), "term");
    auto postings = iterator.value()->postings_with_flags(nullptr, 0);
    ASSERT_TRUE(postings.has_value());
    EXPECT_EQ(postings.value()->next_doc().value(), 0);

    EXPECT_EQ(postings.value()->next_position().value(), 0);
    EXPECT_EQ(postings.value()->start_offset().value(), 5);
    EXPECT_EQ(postings.value()->end_offset().value(), 7);
    auto payload = postings.value()->get_payload();
    ASSERT_TRUE(payload.has_value());
    ASSERT_TRUE(payload.value().has_value());
    EXPECT_EQ(text_of(*payload.value()), "ab");

    // The offset stream chains the deltas from the previous end offset: 7 + 5 = 12.
    EXPECT_EQ(postings.value()->next_position().value(), 3);
    EXPECT_EQ(postings.value()->start_offset().value(), 12);
    EXPECT_EQ(postings.value()->end_offset().value(), 16);
    payload = postings.value()->get_payload();
    ASSERT_TRUE(payload.has_value());
    EXPECT_FALSE(payload.value().has_value());
}

TEST(Lucene90CompressingTermVectorsTest, FinishRejectsAWrongDocumentCount) {
    Fixture fixture;
    auto writer = fixture.writer(4096, 128, 1);
    ASSERT_TRUE(writer != nullptr);
    fixture.write_vector(*writer, {{FREQ_ONLY, apple_terms()}});
    auto finished = writer->finish(2);
    EXPECT_FALSE(finished.has_value());
    EXPECT_NE(finished.error().to_string().find("finish called with numDocs=2"), std::string::npos);
    ASSERT_TRUE(writer->close().has_value());
}

TEST(Lucene90CompressingTermVectorsTest, RejectsInvalidConfiguration) {
    EXPECT_FALSE(Lucene90CompressingTermVectorsFormat::create("X", "", CompressionMode::fast(), 0, 1, 1).has_value());
    EXPECT_FALSE(Lucene90CompressingTermVectorsFormat::create("X", "", CompressionMode::fast(), 1, 0, 1).has_value());
    EXPECT_FALSE(Lucene90CompressingTermVectorsFormat::create("X", "", CompressionMode::fast(), 1, 1, 0).has_value());
    EXPECT_FALSE(BlockPackedWriter::create(0).has_value());
    EXPECT_FALSE(BlockPackedWriter::create(96).has_value());
    EXPECT_TRUE(BlockPackedWriter::create(64).has_value());

    auto format = Lucene90CompressingTermVectorsFormat::create("X", "", CompressionMode::fast(),
                                                              /*chunk_size=*/1 << 12, /*max_docs_per_chunk=*/4,
                                                              /*block_size=*/1);
    ASSERT_TRUE(format.has_value());
    EXPECT_EQ(format.value().name(), "X");
    EXPECT_EQ(format.value().chunk_size(), 1 << 12);
    EXPECT_EQ(format.value().max_docs_per_chunk(), 4);
    EXPECT_EQ(format.value().block_size(), 1);
    EXPECT_NE(format.value().to_string().find("chunkSize=4096"), std::string::npos);
}
