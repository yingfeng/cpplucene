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
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.indexing_chain;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kIdField = "id";
constexpr std::string_view kTitleField = "title";
constexpr std::string_view kBodyField = "body";
constexpr std::string_view kNumField = "num";
constexpr std::string_view kSortedField = "sorted";
constexpr std::string_view kVectorsField = "tv";

std::vector<uint8_t> segment_id(uint8_t seed) {
    std::vector<uint8_t> id(ID_LENGTH, 0);
    id[0] = seed;
    return id;
}

/// Full-text field: postings with positions, optionally stored, optionally with
/// term vectors (which is what a vectors field needs).
FieldType text_type(bool stored, bool vectors = false) {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqsAndPositions).has_value());
    EXPECT_TRUE(type.set_stored(stored).has_value());
    if (vectors) {
        EXPECT_TRUE(type.set_store_term_vectors(true).has_value());
        EXPECT_TRUE(type.set_store_term_vector_positions(true).has_value());
        EXPECT_TRUE(type.set_store_term_vector_offsets(true).has_value());
    }
    return type;
}

FieldType stored_type() {
    FieldType type;
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

FieldType numeric_doc_values_type() {
    FieldType type;
    EXPECT_TRUE(type.set_doc_values_type(DocValuesType::Numeric).has_value());
    return type;
}

FieldType sorted_doc_values_type() {
    FieldType type;
    EXPECT_TRUE(type.set_doc_values_type(DocValuesType::Sorted).has_value());
    return type;
}

std::unique_ptr<Field> text_field(std::string_view name, std::string value, FieldType type) {
    return std::make_unique<Field>(std::string(name), FieldValue{std::move(value)}, std::move(type));
}

std::unique_ptr<Field> stored_field(std::string_view name, std::string value) {
    return text_field(name, std::move(value), stored_type());
}

std::unique_ptr<Field> numeric_field(std::string_view name, int64_t value) {
    return std::make_unique<Field>(std::string(name), FieldValue{Number{value}}, numeric_doc_values_type());
}

std::unique_ptr<Field> sorted_field(std::string_view name, std::string value) {
    std::vector<uint8_t> bytes(value.begin(), value.end());
    return std::make_unique<Field>(std::string(name), FieldValue{std::move(bytes)}, sorted_doc_values_type());
}

/// The three documents the tests index.
std::vector<Document> sample_documents() {
    std::vector<Document> docs;
    {
        Document doc;
        doc.add(stored_field(kIdField, "a"));
        doc.add(text_field(kTitleField, "hello world", text_type(/*stored=*/true)));
        doc.add(stored_field(kBodyField, "first body"));
        doc.add(numeric_field(kNumField, 42));
        doc.add(sorted_field(kSortedField, "a"));
        docs.push_back(std::move(doc));
    }
    {
        Document doc;
        doc.add(stored_field(kIdField, "b"));
        doc.add(text_field(kTitleField, "hello lucene", text_type(/*stored=*/true)));
        doc.add(stored_field(kBodyField, "second body"));
        doc.add(numeric_field(kNumField, 7));
        doc.add(sorted_field(kSortedField, "b"));
        docs.push_back(std::move(doc));
    }
    {
        Document doc;
        doc.add(stored_field(kIdField, "c"));
        doc.add(text_field(kTitleField, "world", text_type(/*stored=*/true)));
        doc.add(numeric_field(kNumField, 100));
        docs.push_back(std::move(doc));
    }
    return docs;
}

/// What one `index_documents` call produced: the directory must outlive the
/// reader, hence the declaration order.
struct Indexed {
    std::unique_ptr<ByteBuffersDirectory> directory;
    std::shared_ptr<DirectoryReader> reader;
};

/// Runs the whole write path: index the documents with [`IndexingChain`], write
/// the metadata the readers need, and open the directory.
Indexed index_documents(std::vector<Document> docs) {
    Indexed indexed;
    indexed.directory = std::make_unique<ByteBuffersDirectory>();
    ByteBuffersDirectory &dir = *indexed.directory;
    const IOContext &context = io_context_default();

    auto created = Lucene101Codec::create();
    EXPECT_TRUE(created.has_value()) << (created ? "" : created.error().to_string());
    if (!created.has_value()) {
        return indexed;
    }
    const Codec &codec = *created.value();

    SegmentInfo info;
    info.name = "_0";
    info.id = segment_id(0x11);
    info.version = Version{10, 1, 0};
    info.min_version = Version{10, 0, 0};
    info.max_doc = static_cast<int32_t>(docs.size());
    info.codec_name = std::string(Lucene101Codec::CODEC_NAME);
    info.diagnostics.emplace("source", "flush");

    WhitespaceAnalyzer analyzer;
    IndexingChain chain(dir, codec, info, LUCENE_CURRENT_VERSION.major, analyzer);
    FieldInfosBuilder builder;
    for (size_t i = 0; i < docs.size(); ++i) {
        auto processed = chain.process_document(static_cast<int32_t>(i), docs[i], builder);
        EXPECT_TRUE(processed.has_value()) << (processed ? "" : processed.error().to_string());
        if (!processed.has_value()) {
            return indexed;
        }
    }
    EXPECT_EQ(chain.num_per_fields(), builder.size());

    auto built = builder.finish();
    EXPECT_TRUE(built.has_value()) << (built ? "" : built.error().to_string());
    if (!built.has_value()) {
        return indexed;
    }
    auto infos = std::make_shared<const FieldInfos>(std::move(built.value()));
    SegmentWriteState state(get_default_info_stream(), dir, infos, context, "");
    auto flushed = chain.flush(state);
    EXPECT_TRUE(flushed.has_value()) << (flushed ? "" : flushed.error().to_string());
    if (!flushed.has_value()) {
        return indexed;
    }

    EXPECT_TRUE(codec.segment_info_format().write(dir, info, context).has_value());
    SegmentCommitInfo commit;
    commit.info = info;

    SegmentInfos segments;
    segments.version = 1;
    segments.counter = 1;
    segments.generation = 1;
    segments.id = segment_id(0x22);
    EXPECT_TRUE(segments.add(std::move(commit)).has_value());
    EXPECT_TRUE(segments.write_to(dir, context).has_value());

    auto reader = DirectoryReader::open(dir);
    EXPECT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    if (reader.has_value()) {
        indexed.reader = std::move(reader.value());
    }
    return indexed;
}

} // namespace

TEST(IndexingChainTest, WritesStoredFieldsPostingsNormsAndDocValues) {
    Indexed indexed = index_documents(sample_documents());
    ASSERT_NE(indexed.reader, nullptr);
    DirectoryReader &reader = *indexed.reader;

    EXPECT_EQ(reader.max_doc().value(), 3);
    EXPECT_EQ(reader.num_docs().value(), 3);
    ASSERT_EQ(reader.leaves().value().size(), 1U);
    LeafReader &leaf = *reader.leaves().value()[0].reader();

    // Stored fields come back verbatim.
    auto stored = leaf.stored_fields();
    ASSERT_TRUE(stored.has_value());
    auto doc = stored.value()->document(0);
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc.value().get(kIdField).value().value(), "a");
    EXPECT_EQ(doc.value().get(kTitleField).value().value(), "hello world");
    EXPECT_EQ(doc.value().get(kBodyField).value().value(), "first body");

    // The third document has no `body`.
    auto third = stored.value()->document(2);
    ASSERT_TRUE(third.has_value());
    EXPECT_TRUE(third.value().get(kBodyField).value() == std::nullopt);

    // Postings: "hello" is in documents 0 and 1, "world" in 0 and 2.
    EXPECT_EQ(leaf.doc_freq(Term(std::string(kTitleField), BytesRef::from_string("hello"))).value(), 2);
    EXPECT_EQ(leaf.doc_freq(Term(std::string(kTitleField), BytesRef::from_string("world"))).value(), 2);
    EXPECT_EQ(leaf.doc_freq(Term(std::string(kTitleField), BytesRef::from_string("lucene"))).value(), 1);
    EXPECT_EQ(leaf.doc_freq(Term(std::string(kTitleField), BytesRef::from_string("missing"))).value(), 0);

    // The chain computed a norm per document, and the codec wrote them.
    auto norms = leaf.get_norm_values(kTitleField);
    ASSERT_TRUE(norms.has_value()) << (norms ? "" : norms.error().to_string());
    ASSERT_NE(norms.value(), nullptr);
    for (int32_t doc_id = 0; doc_id < 3; ++doc_id) {
        ASSERT_EQ(norms.value()->advance(doc_id).value(), doc_id);
        EXPECT_GT(norms.value()->long_value().value(), 0);
    }
    // The stored-only `id` field has no norms at all.
    EXPECT_EQ(leaf.get_norm_values(kIdField).value(), nullptr);

    // Numeric doc values.
    auto numbers = leaf.get_numeric_doc_values(kNumField);
    ASSERT_TRUE(numbers.has_value());
    ASSERT_NE(numbers.value(), nullptr);
    const std::vector<int64_t> expected{42, 7, 100};
    for (int32_t doc_id = 0; doc_id < 3; ++doc_id) {
        ASSERT_EQ(numbers.value()->advance(doc_id).value(), doc_id);
        EXPECT_EQ(numbers.value()->long_value().value(), expected[static_cast<size_t>(doc_id)]);
    }

    // Sorted doc values: two distinct values, one per document.
    auto sorted = leaf.get_sorted_doc_values(kSortedField);
    ASSERT_TRUE(sorted.has_value());
    ASSERT_NE(sorted.value(), nullptr);
    EXPECT_EQ(sorted.value()->get_value_count().value(), 2);
    ASSERT_EQ(sorted.value()->advance(0).value(), 0);
    EXPECT_EQ(sorted.value()->ord_value().value(), 0);
    ASSERT_EQ(sorted.value()->advance(1).value(), 1);
    EXPECT_EQ(sorted.value()->ord_value().value(), 1);
    // Only the first two documents have a value.
    EXPECT_EQ(sorted.value()->advance(2).value(), NO_MORE_DOCS);

    EXPECT_TRUE(leaf.terms(kTitleField).has_value());
}

TEST(IndexingChainTest, IndexesIntoASearchableSegment) {
    Indexed indexed = index_documents(sample_documents());
    ASSERT_NE(indexed.reader, nullptr);

    IndexSearcher searcher(indexed.reader);
    TermQuery query(Term(std::string(kTitleField), BytesRef::from_string("hello")));
    auto top = searcher.search(query, 10);
    ASSERT_TRUE(top.has_value()) << (top ? "" : top.error().to_string());
    EXPECT_EQ(top.value().total_hits.value, 2);
    ASSERT_EQ(top.value().score_docs.size(), 2U);
    std::vector<int32_t> hits;
    for (const auto &hit : top.value().score_docs) {
        hits.push_back(hit.doc);
        EXPECT_GT(hit.score, 0.0F);
    }
    EXPECT_EQ(hits, (std::vector<int32_t>{0, 1}));
}

TEST(IndexingChainTest, WritesTermVectors) {
    std::vector<Document> docs;
    {
        Document doc;
        doc.add(stored_field(kIdField, "a"));
        doc.add(text_field(kVectorsField, "alpha beta", text_type(/*stored=*/false, /*vectors=*/true)));
        docs.push_back(std::move(doc));
    }
    {
        Document doc;
        doc.add(stored_field(kIdField, "b"));
        doc.add(text_field(kVectorsField, "beta gamma", text_type(/*stored=*/false, /*vectors=*/true)));
        docs.push_back(std::move(doc));
    }

    Indexed indexed = index_documents(std::move(docs));
    ASSERT_NE(indexed.reader, nullptr);
    LeafReader &leaf = *indexed.reader->leaves().value()[0].reader();
    EXPECT_TRUE(leaf.field_infos().value()->by_name(kVectorsField)->store_term_vector);

    auto vectors = leaf.term_vectors();
    ASSERT_TRUE(vectors.has_value());
    auto doc_vectors = vectors.value()->get(0);
    ASSERT_TRUE(doc_vectors.has_value());
    ASSERT_NE(doc_vectors.value(), nullptr);
    auto terms = doc_vectors.value()->terms(kVectorsField);
    ASSERT_TRUE(terms.has_value());
    ASSERT_NE(terms.value(), nullptr);
    auto iterator = terms.value()->iterator();
    ASSERT_TRUE(iterator.has_value());
    std::vector<std::string> read;
    for (;;) {
        auto term = iterator.value()->next();
        ASSERT_TRUE(term.has_value());
        if (term.value() == nullptr) {
            break;
        }
        const auto slice = term.value()->as_slice();
        read.emplace_back(reinterpret_cast<const char *>(slice.data()), slice.size());
    }
    EXPECT_EQ(read, (std::vector<std::string>{"alpha", "beta"}));
}

TEST(IndexingChainTest, RejectsAnInconsistentSchema) {
    auto created = Lucene101Codec::create();
    ASSERT_TRUE(created.has_value());
    ByteBuffersDirectory dir;
    SegmentInfo info;
    info.name = "_0";
    info.id = segment_id(0x33);
    info.max_doc = 2;
    info.codec_name = std::string(Lucene101Codec::CODEC_NAME);

    WhitespaceAnalyzer analyzer;
    IndexingChain chain(dir, *created.value(), info, LUCENE_CURRENT_VERSION.major, analyzer);
    FieldInfosBuilder builder;

    Document first;
    first.add(text_field(kTitleField, "hello", text_type(/*stored=*/false)));
    ASSERT_TRUE(chain.process_document(0, first, builder).has_value());

    // Same name, different options: the segment cannot mix both.
    Document second;
    FieldType other;
    ASSERT_TRUE(other.set_index_options(IndexOptions::Docs).has_value());
    second.add(text_field(kTitleField, "hello", std::move(other)));
    auto rejected = chain.process_document(1, second, builder);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().kind(), ErrorKind::kIllegalArgument);
    EXPECT_NE(rejected.error().to_string().find("Inconsistency"), std::string::npos);
    EXPECT_NE(rejected.error().to_string().find("index options"), std::string::npos);
}

TEST(IndexingChainTest, AcceptsPointsAndRejectsVectors) {
    auto created = Lucene101Codec::create();
    ASSERT_TRUE(created.has_value());
    ByteBuffersDirectory dir;
    SegmentInfo info;
    info.name = "_0";
    info.id = segment_id(0x44);
    info.max_doc = 2;
    info.codec_name = std::string(Lucene101Codec::CODEC_NAME);

    WhitespaceAnalyzer analyzer;
    IndexingChain chain(dir, *created.value(), info, LUCENE_CURRENT_VERSION.major, analyzer);
    FieldInfosBuilder builder;

    Document points;
    FieldType point_type;
    ASSERT_TRUE(point_type.set_dimensions(1, 8).has_value());
    points.add(std::make_unique<Field>(std::string("p"), FieldValue{std::vector<uint8_t>(8, 0)},
                                      std::move(point_type)));
    // Points are indexed; a field carrying the right number of bytes is accepted.
    EXPECT_TRUE(chain.process_document(0, points, builder).has_value());

    Document vectors;
    FieldType vector_type;
    ASSERT_TRUE(vector_type.set_vector_attributes(4, VectorEncoding::Float32,
                                                  VectorSimilarityFunction::Euclidean)
                        .has_value());
    vectors.add(std::make_unique<Field>(std::string("v"), FieldValue{std::vector<uint8_t>(16, 0)},
                                       std::move(vector_type)));
    auto rejected_vectors = chain.process_document(1, vectors, builder);
    ASSERT_FALSE(rejected_vectors.has_value());
    EXPECT_EQ(rejected_vectors.error().kind(), ErrorKind::kUnsupportedOperation);
}

TEST(IndexingChainTest, AbortDropsTheBuffers) {
    auto created = Lucene101Codec::create();
    ASSERT_TRUE(created.has_value());
    ByteBuffersDirectory dir;
    SegmentInfo info;
    info.name = "_0";
    info.id = segment_id(0x55);
    info.max_doc = 1;
    info.codec_name = std::string(Lucene101Codec::CODEC_NAME);

    WhitespaceAnalyzer analyzer;
    IndexingChain chain(dir, *created.value(), info, LUCENE_CURRENT_VERSION.major, analyzer);
    FieldInfosBuilder builder;

    Document doc;
    doc.add(text_field(kTitleField, "hello world", text_type(/*stored=*/true)));
    ASSERT_TRUE(chain.process_document(0, doc, builder).has_value());
    EXPECT_EQ(chain.num_per_fields(), 1U);
    EXPECT_TRUE(chain.abort().has_value());
    // The field table survives (it describes the segment), the buffers do not.
    EXPECT_EQ(chain.num_per_fields(), 1U);
}
