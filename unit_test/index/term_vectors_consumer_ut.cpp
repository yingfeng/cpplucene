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
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.term_vectors_consumer;

import lucene_core;

using namespace lucene;

namespace {

std::string text_of(const BytesRef &term) {
    return std::string(reinterpret_cast<const char *>(term.bytes().data() + term.offset()), term.length());
}

constexpr std::string_view FORMAT_NAME = "Lucene90TermVectorsConsumerTest";

/// Drives a [`TermVectorsConsumer`] the way the indexing chain does: it owns
/// the per-field writers, the shared term pool and the term-vectors pools.
struct Driver {
    Driver(size_t num_fields, int32_t max_doc) : format(make_format()) {
        segment.name = "_0";
        segment.id.assign(ID_LENGTH, 0x44);
        segment.max_doc = max_doc;
        for (size_t i = 0; i < num_fields; ++i) {
            auto info = std::make_shared<FieldInfo>();
            info->number = static_cast<int32_t>(i);
            info->name = "field" + std::to_string(i);
            info->index_options = IndexOptions::DocsAndFreqsAndPositionsAndOffsets;
            infos.push_back(std::move(info));
        }
        consumer = std::make_unique<TermVectorsConsumer>(directory, *format, segment);
    }

    static std::unique_ptr<Lucene90CompressingTermVectorsFormat> make_format() {
        auto created = Lucene90CompressingTermVectorsFormat::create(std::string(FORMAT_NAME), "",
                                                                   CompressionMode::fast(), 4096, 128, 4);
        EXPECT_TRUE(created.has_value()) << (created ? "" : created.error().to_string());
        if (!created.has_value()) {
            return nullptr;
        }
        return std::make_unique<Lucene90CompressingTermVectorsFormat>(std::move(created.value()));
    }

    /// Term text is interned once per distinct term, exactly like the postings
    /// hash does before the vectors hash keys it by pool offset.
    int32_t intern(std::string_view term) {
        if (auto it = term_starts.find(std::string(term)); it != term_starts.end()) {
            return it->second;
        }
        auto added = text_pool.add_bytes_ref(BytesRef::from_string(term), term_pool);
        EXPECT_TRUE(added.has_value());
        const int32_t start = added.has_value() ? added.value() : 0;
        term_starts.emplace(std::string(term), start);
        return start;
    }

    /// Creates the per-field writer; `stores_vectors` mirrors the schema flag
    /// the indexing chain sets on the [`FieldInfo`] before the field is added.
    [[nodiscard]] TermVectorsConsumerPerField *add_field(size_t index, bool stores_vectors = true) {
        infos[index]->store_term_vector = stores_vectors;
        auto created = consumer->add_field(infos[index]);
        EXPECT_TRUE(created.has_value()) << (created ? "" : created.error().to_string());
        return created.has_value() ? created.value() : nullptr;
    }

    /// A field whose type stores term vectors with positions and offsets.
    [[nodiscard]] static Field vector_field(size_t index) {
        FieldType type;
        EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqsAndPositionsAndOffsets).has_value());
        EXPECT_TRUE(type.set_store_term_vectors(true).has_value());
        EXPECT_TRUE(type.set_store_term_vector_positions(true).has_value());
        EXPECT_TRUE(type.set_store_term_vector_offsets(true).has_value());
        return Field("field" + std::to_string(index), std::string("value"), std::move(type));
    }

    void add_token(TermVectorsConsumerPerField &per_field, std::string_view term, int32_t doc_id,
                   FieldInvertState &state, AttributeSource &attributes, int32_t position, int32_t start,
                   int32_t end) {
        state.set_position(position);
        attributes.get_attribute<OffsetAttribute>()->set_offset(static_cast<uint32_t>(start),
                                                               static_cast<uint32_t>(end));
        const auto text_start = intern(term);
        ASSERT_TRUE(per_field
                            .add_with_text_start(text_start, doc_id, state, attributes, tv_int_pool, tv_byte_pool,
                                                 term_pool)
                            .has_value());
    }

    /// Closes the codec writer, then builds the field infos a reader needs.
    [[nodiscard]] std::shared_ptr<const FieldInfos> flush(int32_t max_doc) {
        EXPECT_TRUE(consumer->flush(max_doc).has_value());
        std::vector<FieldInfo> fields;
        for (const auto &info : infos) {
            fields.push_back(*info);
        }
        auto built = FieldInfos::of(std::move(fields));
        EXPECT_TRUE(built.has_value());
        return built.has_value() ? std::make_shared<const FieldInfos>(std::move(built.value())) : nullptr;
    }

    [[nodiscard]] std::unique_ptr<TermVectorsReader> reader(const std::shared_ptr<const FieldInfos> &field_infos) {
        auto created = format->vectors_reader(directory, segment, field_infos, io_context_default());
        EXPECT_TRUE(created.has_value()) << (created ? "" : created.error().to_string());
        return created.has_value() ? std::move(created.value()) : nullptr;
    }

    ByteBuffersDirectory directory;
    std::unique_ptr<Lucene90CompressingTermVectorsFormat> format;
    SegmentInfo segment;
    std::unique_ptr<TermVectorsConsumer> consumer;
    std::vector<std::shared_ptr<FieldInfo>> infos;

    BytesRefBlockPool text_pool;
    ByteBlockPool term_pool;
    ByteBlockPool tv_byte_pool;
    IntBlockPool tv_int_pool;
    std::map<std::string, int32_t> term_starts;
};

/// Walks the terms of `field_name` in `doc`; returns `(term, freq)`.
std::vector<std::pair<std::string, int32_t>> terms_of(TermVectorsReader &reader, int32_t doc,
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
        // `total_term_freq` is the number of occurrences, `doc_freq` only the
        // number of documents that contain the term.
        auto freq = iterator.value()->total_term_freq();
        EXPECT_TRUE(freq.has_value());
        result.emplace_back(text_of(*term.value()), freq.has_value() ? freq.value() : 0);
    }
    return result;
}

} // namespace

TEST(TermVectorsConsumerTest, WritesPositionsAndOffsetsOfOneDocument) {
    Driver driver(/*num_fields=*/1, /*max_doc=*/1);
    auto *per_field = driver.add_field(0);
    ASSERT_TRUE(per_field != nullptr);

    AttributeSource attributes;
    attributes.add_attribute<OffsetAttribute>();
    attributes.add_attribute<PayloadAttribute>();
    FieldInvertState state;
    const Field field = Driver::vector_field(0);

    ASSERT_TRUE(driver.consumer->start_document().has_value());
    ASSERT_TRUE(per_field->start(field, true, driver.term_pool).has_value());

    // Out of order on purpose: the writer sorts the terms before writing.
    driver.add_token(*per_field, "beta", 0, state, attributes, /*position=*/1, /*start=*/6, /*end=*/10);
    driver.add_token(*per_field, "alpha", 0, state, attributes, /*position=*/0, /*start=*/0, /*end=*/5);
    driver.add_token(*per_field, "alpha", 0, state, attributes, /*position=*/2, /*start=*/11, /*end=*/16);

    ASSERT_TRUE(per_field->finish(0).has_value());
    EXPECT_TRUE(per_field->pending_flush());
    ASSERT_TRUE(driver.consumer->finish_document(0, driver.tv_int_pool, driver.tv_byte_pool, driver.term_pool)
                        .has_value());
    EXPECT_FALSE(per_field->pending_flush());

    auto infos = driver.flush(1);
    ASSERT_TRUE(infos != nullptr);
    EXPECT_TRUE(infos->by_name("field0")->store_term_vector);

    auto reader = driver.reader(infos);
    ASSERT_TRUE(reader != nullptr);

    const auto terms = terms_of(*reader, 0, "field0");
    ASSERT_EQ(terms.size(), 2);
    EXPECT_EQ(terms[0].first, "alpha");
    EXPECT_EQ(terms[0].second, 2);
    EXPECT_EQ(terms[1].first, "beta");
    EXPECT_EQ(terms[1].second, 1);

    auto vectors = reader->get(0);
    ASSERT_TRUE(vectors.has_value());
    auto field_terms = vectors.value()->terms("field0");
    ASSERT_TRUE(field_terms.has_value());
    EXPECT_TRUE(field_terms.value()->has_positions());
    EXPECT_TRUE(field_terms.value()->has_offsets());
    EXPECT_FALSE(field_terms.value()->has_payloads());

    auto iterator = field_terms.value()->iterator();
    ASSERT_TRUE(iterator.has_value());
    ASSERT_TRUE(iterator.value()->next().has_value());
    auto postings = iterator.value()->postings_with_flags(nullptr, 0);
    ASSERT_TRUE(postings.has_value()) << (postings ? "" : postings.error().to_string());
    EXPECT_EQ(postings.value()->next_doc().value(), 0);
    EXPECT_EQ(postings.value()->freq().value(), 2);

    EXPECT_EQ(postings.value()->next_position().value(), 0);
    EXPECT_EQ(postings.value()->start_offset().value(), 0);
    EXPECT_EQ(postings.value()->end_offset().value(), 5);
    EXPECT_EQ(postings.value()->next_position().value(), 2);
    EXPECT_EQ(postings.value()->start_offset().value(), 11);
    EXPECT_EQ(postings.value()->end_offset().value(), 16);
    EXPECT_FALSE(postings.value()->next_position().has_value());

    EXPECT_TRUE(reader->check_integrity().has_value());
}

TEST(TermVectorsConsumerTest, WritesPayloadsWhenTheFieldAsksForThem) {
    Driver driver(1, 1);
    auto *per_field = driver.add_field(0);
    ASSERT_TRUE(per_field != nullptr);

    FieldType type;
    ASSERT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqsAndPositionsAndOffsets).has_value());
    ASSERT_TRUE(type.set_store_term_vectors(true).has_value());
    ASSERT_TRUE(type.set_store_term_vector_positions(true).has_value());
    ASSERT_TRUE(type.set_store_term_vector_offsets(true).has_value());
    ASSERT_TRUE(type.set_store_term_vector_payloads(true).has_value());
    const Field field("field0", std::string("value"), std::move(type));

    AttributeSource attributes;
    attributes.add_attribute<OffsetAttribute>();
    auto *payload = attributes.add_attribute<PayloadAttribute>();
    FieldInvertState state;

    ASSERT_TRUE(driver.consumer->start_document().has_value());
    ASSERT_TRUE(per_field->start(field, true, driver.term_pool).has_value());
    payload->set_payload(BytesRef::from_string("p1"));
    driver.add_token(*per_field, "alpha", 0, state, attributes, 0, 0, 5);
    ASSERT_TRUE(per_field->finish(0).has_value());
    ASSERT_TRUE(driver.consumer->finish_document(0, driver.tv_int_pool, driver.tv_byte_pool, driver.term_pool)
                        .has_value());

    auto infos = driver.flush(1);
    auto reader = driver.reader(infos);
    ASSERT_TRUE(reader != nullptr);

    auto vectors = reader->get(0);
    ASSERT_TRUE(vectors.has_value());
    auto field_terms = vectors.value()->terms("field0");
    ASSERT_TRUE(field_terms.has_value());
    EXPECT_TRUE(field_terms.value()->has_payloads());

    auto iterator = field_terms.value()->iterator();
    ASSERT_TRUE(iterator.has_value());
    ASSERT_TRUE(iterator.value()->next().has_value());
    auto postings = iterator.value()->postings_with_flags(nullptr, 0);
    ASSERT_TRUE(postings.has_value());
    ASSERT_EQ(postings.value()->next_doc().value(), 0);
    ASSERT_EQ(postings.value()->next_position().value(), 0);
    auto read = postings.value()->get_payload();
    ASSERT_TRUE(read.has_value());
    ASSERT_TRUE(read.value().has_value());
    const auto payload_bytes = read.value().value();
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(payload_bytes.data()), payload_bytes.size()), "p1");
}

TEST(TermVectorsConsumerTest, FieldsWithoutVectorsAreSkipped) {
    Driver driver(/*num_fields=*/2, /*max_doc=*/1);
    auto *plain = driver.add_field(/*index=*/0, /*stores_vectors=*/false);
    auto *vectored = driver.add_field(1);
    ASSERT_TRUE(plain != nullptr);
    ASSERT_TRUE(vectored != nullptr);

    FieldType plain_type;
    ASSERT_TRUE(plain_type.set_index_options(IndexOptions::DocsAndFreqsAndPositions).has_value());
    const Field plain_field("field0", std::string("value"), std::move(plain_type));
    const Field vectored_field = Driver::vector_field(1);

    AttributeSource attributes;
    attributes.add_attribute<OffsetAttribute>();
    FieldInvertState state;

    ASSERT_TRUE(driver.consumer->start_document().has_value());
    ASSERT_TRUE(plain->start(plain_field, true, driver.term_pool).has_value());
    EXPECT_FALSE(plain->do_vectors());
    driver.add_token(*plain, "alpha", 0, state, attributes, 0, 0, 5);
    ASSERT_TRUE(plain->finish(0).has_value());
    EXPECT_FALSE(plain->pending_flush());

    ASSERT_TRUE(vectored->start(vectored_field, true, driver.term_pool).has_value());
    EXPECT_TRUE(vectored->do_vectors());
    driver.add_token(*vectored, "beta", 0, state, attributes, 0, 0, 4);
    ASSERT_TRUE(vectored->finish(1).has_value());

    ASSERT_TRUE(driver.consumer->finish_document(0, driver.tv_int_pool, driver.tv_byte_pool, driver.term_pool)
                        .has_value());

    auto infos = driver.flush(1);
    EXPECT_FALSE(infos->by_name("field0")->store_term_vector);
    EXPECT_TRUE(infos->by_name("field1")->store_term_vector);

    auto reader = driver.reader(infos);
    ASSERT_TRUE(reader != nullptr);
    auto vectors = reader->get(0);
    ASSERT_TRUE(vectors.has_value());
    EXPECT_EQ(vectors.value()->size().value(), 1);
    EXPECT_TRUE(vectors.value()->terms("field0").value() == nullptr);

    // The vectors hash of the plain field was still fed by the chain, but
    // nothing of it was written.
    const auto read = terms_of(*reader, 0, "field1");
    ASSERT_EQ(read.size(), 1);
    EXPECT_EQ(read[0].first, "beta");
}

TEST(TermVectorsConsumerTest, FillsDocumentsWithoutVectors) {
    Driver driver(/*num_fields=*/1, /*max_doc=*/3);
    auto *per_field = driver.add_field(0);
    ASSERT_TRUE(per_field != nullptr);

    AttributeSource attributes;
    attributes.add_attribute<OffsetAttribute>();
    FieldInvertState state;
    const Field field = Driver::vector_field(0);

    // Document 0 has a vector, documents 1 and 2 do not, but the codec writer
    // still needs one `start_document`/`finish_document` pair per document.
    ASSERT_TRUE(driver.consumer->start_document().has_value());
    ASSERT_TRUE(per_field->start(field, true, driver.term_pool).has_value());
    driver.add_token(*per_field, "alpha", 0, state, attributes, 0, 0, 5);
    ASSERT_TRUE(per_field->finish(0).has_value());
    ASSERT_TRUE(driver.consumer->finish_document(0, driver.tv_int_pool, driver.tv_byte_pool, driver.term_pool)
                        .has_value());

    ASSERT_TRUE(driver.consumer->start_document().has_value());
    ASSERT_TRUE(driver.consumer->finish_document(1, driver.tv_int_pool, driver.tv_byte_pool, driver.term_pool)
                        .has_value());
    ASSERT_TRUE(driver.consumer->finish_document(2, driver.tv_int_pool, driver.tv_byte_pool, driver.term_pool)
                        .has_value());

    auto infos = driver.flush(3);
    auto reader = driver.reader(infos);
    ASSERT_TRUE(reader != nullptr);

    for (int32_t doc = 1; doc < 3; ++doc) {
        auto vectors = reader->get(doc);
        ASSERT_TRUE(vectors.has_value()) << (vectors ? "" : vectors.error().to_string());
        // A document without vectors is reported as "no fields" (`nullptr`),
        // which is what the codec does when the field count of the doc is 0.
        EXPECT_TRUE(vectors.value() == nullptr);
    }
    EXPECT_TRUE(reader->check_integrity().has_value());
}

TEST(TermVectorsConsumerTest, WritesFieldsInNameOrder) {
    Driver driver(/*num_fields=*/2, /*max_doc=*/1);
    driver.infos[0]->name = "zeta";
    driver.infos[1]->name = "alpha";

    auto *zeta = driver.add_field(0);
    auto *alpha = driver.add_field(1);
    ASSERT_TRUE(zeta != nullptr);
    ASSERT_TRUE(alpha != nullptr);

    FieldType type;
    ASSERT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqsAndPositionsAndOffsets).has_value());
    ASSERT_TRUE(type.set_store_term_vectors(true).has_value());
    const Field zeta_field("zeta", std::string("value"), FieldType::from(type));
    const Field alpha_field("alpha", std::string("value"), std::move(type));

    AttributeSource attributes;
    attributes.add_attribute<OffsetAttribute>();
    FieldInvertState state;

    ASSERT_TRUE(driver.consumer->start_document().has_value());
    ASSERT_TRUE(zeta->start(zeta_field, true, driver.term_pool).has_value());
    driver.add_token(*zeta, "zulu", 0, state, attributes, 0, 0, 4);
    ASSERT_TRUE(zeta->finish(0).has_value());

    ASSERT_TRUE(alpha->start(alpha_field, true, driver.term_pool).has_value());
    driver.add_token(*alpha, "alfa", 0, state, attributes, 0, 0, 4);
    ASSERT_TRUE(alpha->finish(1).has_value());

    ASSERT_TRUE(driver.consumer->finish_document(0, driver.tv_int_pool, driver.tv_byte_pool, driver.term_pool)
                        .has_value());

    auto infos = driver.flush(1);
    auto reader = driver.reader(infos);
    ASSERT_TRUE(reader != nullptr);

    auto vectors = reader->get(0);
    ASSERT_TRUE(vectors.has_value());
    auto iterator = vectors.value()->iterator();
    ASSERT_TRUE(iterator.has_value());
    std::vector<std::string> names;
    for (;;) {
        auto name = iterator.value()->next();
        ASSERT_TRUE(name.has_value());
        if (name.value() == nullptr) {
            break;
        }
        names.push_back(std::string(*name.value()));
    }
    EXPECT_EQ(names, (std::vector<std::string>{"alpha", "zeta"}));
}

TEST(TermVectorsConsumerTest, RejectsVectorSettingsWithoutVectors) {
    Driver driver(1, 1);
    auto *per_field = driver.add_field(0);
    ASSERT_TRUE(per_field != nullptr);

    FieldType type;
    ASSERT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqsAndPositions).has_value());
    ASSERT_TRUE(type.set_store_term_vector_positions(true).has_value());
    const Field field("field0", std::string("value"), std::move(type));

    ASSERT_TRUE(driver.consumer->start_document().has_value());
    auto started = per_field->start(field, true, driver.term_pool);
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(TermVectorsConsumerTest, RejectsInconsistentSettingsOfTheSameField) {
    Driver driver(1, 1);
    auto *per_field = driver.add_field(0);
    ASSERT_TRUE(per_field != nullptr);

    AttributeSource attributes;
    attributes.add_attribute<OffsetAttribute>();
    FieldInvertState state;

    ASSERT_TRUE(driver.consumer->start_document().has_value());
    const Field first = Driver::vector_field(0);
    ASSERT_TRUE(per_field->start(first, true, driver.term_pool).has_value());
    driver.add_token(*per_field, "alpha", 0, state, attributes, 0, 0, 5);

    // The second value of the same field must carry the same vector settings.
    FieldType inconsistent;
    ASSERT_TRUE(inconsistent.set_index_options(IndexOptions::DocsAndFreqsAndPositionsAndOffsets).has_value());
    ASSERT_TRUE(inconsistent.set_store_term_vectors(true).has_value());
    const Field second("field0", std::string("value"), std::move(inconsistent));

    auto started = per_field->start(second, false, driver.term_pool);
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(TermVectorsConsumerTest, RejectsUnindexedFields) {
    Driver driver(1, 1);
    driver.infos[0]->index_options = IndexOptions::None;

    auto created = driver.consumer->add_field(driver.infos[0]);
    ASSERT_FALSE(created.has_value());
    EXPECT_EQ(created.error().kind(), ErrorKind::kIllegalArgument);
    EXPECT_EQ(driver.consumer->num_per_fields(), 0);
}

TEST(TermVectorsConsumerTest, FlushWithoutVectorsWritesNothing) {
    Driver driver(1, 1);
    ASSERT_FALSE(driver.consumer->is_initialized());
    EXPECT_TRUE(driver.consumer->flush(1).has_value());
    EXPECT_FALSE(driver.consumer->is_initialized());
    auto files = driver.directory.list_all();
    ASSERT_TRUE(files.has_value());
    EXPECT_TRUE(files.value().empty());
}

TEST(TermVectorsConsumerTest, AbortClosesTheWriter) {
    Driver driver(1, 1);
    auto *per_field = driver.add_field(0);
    ASSERT_TRUE(per_field != nullptr);

    AttributeSource attributes;
    attributes.add_attribute<OffsetAttribute>();
    FieldInvertState state;
    const Field field = Driver::vector_field(0);

    ASSERT_TRUE(driver.consumer->start_document().has_value());
    ASSERT_TRUE(per_field->start(field, true, driver.term_pool).has_value());
    driver.add_token(*per_field, "alpha", 0, state, attributes, 0, 0, 5);
    ASSERT_TRUE(per_field->finish(0).has_value());
    ASSERT_TRUE(driver.consumer->finish_document(0, driver.tv_int_pool, driver.tv_byte_pool, driver.term_pool)
                        .has_value());
    EXPECT_TRUE(driver.consumer->is_initialized());

    ASSERT_TRUE(driver.consumer->abort(driver.tv_int_pool, driver.tv_byte_pool).has_value());
    EXPECT_FALSE(driver.consumer->is_initialized());
}
