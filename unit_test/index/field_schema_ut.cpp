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
#include <string>

module lucene_core:ut.field_schema;

import lucene_core;

using namespace lucene;

namespace {

/// A `FieldInfo` matching what `initialize_field_info` builds for a plain
/// full-text field.
FieldInfo text_field_info() {
    FieldInfo info;
    info.number = 0;
    info.name = "title";
    info.index_options = IndexOptions::DocsAndFreqsAndPositions;
    return info;
}

} // namespace

TEST(FieldSchemaTest, DefaultsAndReset) {
    FieldSchema schema("title");
    EXPECT_EQ(schema.name(), "title");
    EXPECT_EQ(schema.doc_id(), 0);
    EXPECT_EQ(schema.index_options(), IndexOptions::None);
    EXPECT_FALSE(schema.omit_norms());
    EXPECT_EQ(schema.doc_values_type(), DocValuesType::None);

    ASSERT_TRUE(schema.set_index_options(IndexOptions::DocsAndFreqs, true, false).has_value());
    ASSERT_TRUE(schema.set_doc_values(DocValuesType::Numeric, DocValuesSkipIndexType::Range).has_value());
    ASSERT_TRUE(schema.set_points(1, 1, 8).has_value());
    ASSERT_TRUE(schema.set_vectors(VectorEncoding::Byte, VectorSimilarityFunction::Cosine, 4).has_value());

    schema.reset(7);
    EXPECT_EQ(schema.doc_id(), 7);
    EXPECT_EQ(schema.index_options(), IndexOptions::None);
    EXPECT_FALSE(schema.omit_norms());
    EXPECT_EQ(schema.doc_values_type(), DocValuesType::None);
    EXPECT_EQ(schema.doc_values_skip_index(), DocValuesSkipIndexType::None);
    EXPECT_EQ(schema.point_dimension_count(), 0);
    EXPECT_EQ(schema.point_num_bytes(), 0);
    EXPECT_EQ(schema.vector_dimension(), 0);
    EXPECT_EQ(schema.vector_encoding(), VectorEncoding::Float32);
    EXPECT_EQ(schema.vector_similarity_function(), VectorSimilarityFunction::Euclidean);
}

TEST(FieldSchemaTest, FirstInstanceWins) {
    FieldSchema schema("title");
    ASSERT_TRUE(schema.set_index_options(IndexOptions::DocsAndFreqsAndPositions, false, true).has_value());
    // The same options are accepted again (multi-valued field).
    EXPECT_TRUE(schema.set_index_options(IndexOptions::DocsAndFreqsAndPositions, false, true).has_value());
    EXPECT_EQ(schema.index_options(), IndexOptions::DocsAndFreqsAndPositions);
    EXPECT_TRUE(schema.store_term_vector());

    ASSERT_TRUE(schema.set_doc_values(DocValuesType::SortedSet, DocValuesSkipIndexType::None).has_value());
    EXPECT_TRUE(schema.set_doc_values(DocValuesType::SortedSet, DocValuesSkipIndexType::None).has_value());
    EXPECT_EQ(schema.doc_values_type(), DocValuesType::SortedSet);

    ASSERT_TRUE(schema.set_points(2, 1, 16).has_value());
    EXPECT_TRUE(schema.set_points(2, 1, 16).has_value());
    EXPECT_EQ(schema.point_dimension_count(), 2);

    ASSERT_TRUE(schema.set_vectors(VectorEncoding::Float32, VectorSimilarityFunction::DotProduct, 8).has_value());
    EXPECT_TRUE(schema.set_vectors(VectorEncoding::Float32, VectorSimilarityFunction::DotProduct, 8).has_value());
    EXPECT_EQ(schema.vector_dimension(), 8);
}

TEST(FieldSchemaTest, ConflictingInstancesAreRejected) {
    FieldSchema schema("title");
    schema.reset(3);
    ASSERT_TRUE(schema.set_index_options(IndexOptions::DocsAndFreqsAndPositions, false, false).has_value());

    auto options = schema.set_index_options(IndexOptions::Docs, false, false);
    ASSERT_FALSE(options.has_value());
    EXPECT_EQ(options.error().kind(), ErrorKind::kIllegalArgument);
    // The message names the field, the document and the value that differs.
    const auto message = options.error().to_string();
    EXPECT_NE(message.find("title"), std::string::npos);
    EXPECT_NE(message.find("DOCS_AND_FREQS_AND_POSITIONS"), std::string::npos);
    EXPECT_NE(message.find("DOCS"), std::string::npos);

    auto norms = schema.set_index_options(IndexOptions::DocsAndFreqsAndPositions, true, false);
    ASSERT_FALSE(norms.has_value());
    auto vectors = schema.set_index_options(IndexOptions::DocsAndFreqsAndPositions, false, true);
    ASSERT_FALSE(vectors.has_value());

    ASSERT_TRUE(schema.set_doc_values(DocValuesType::Numeric, DocValuesSkipIndexType::None).has_value());
    auto doc_values = schema.set_doc_values(DocValuesType::Sorted, DocValuesSkipIndexType::None);
    ASSERT_FALSE(doc_values.has_value());
    auto skip_index = schema.set_doc_values(DocValuesType::Numeric, DocValuesSkipIndexType::Range);
    ASSERT_FALSE(skip_index.has_value());

    ASSERT_TRUE(schema.set_points(1, 1, 8).has_value());
    EXPECT_FALSE(schema.set_points(2, 1, 8).has_value());
    EXPECT_FALSE(schema.set_points(1, 2, 8).has_value());

    ASSERT_TRUE(schema.set_vectors(VectorEncoding::Byte, VectorSimilarityFunction::Cosine, 4).has_value());
    EXPECT_FALSE(schema.set_vectors(VectorEncoding::Float32, VectorSimilarityFunction::Cosine, 4).has_value());
    EXPECT_FALSE(schema.set_vectors(VectorEncoding::Byte, VectorSimilarityFunction::Euclidean, 4).has_value());
    EXPECT_FALSE(schema.set_vectors(VectorEncoding::Byte, VectorSimilarityFunction::Cosine, 8).has_value());
}

TEST(FieldSchemaTest, AttributesAreMerged) {
    FieldSchema schema("title");
    EXPECT_TRUE(schema.update_attributes({{"softDeletesField", "x"}}).has_value());
    EXPECT_TRUE(schema.update_attributes({{"softDeletesField", "y"}, {"other", "z"}}).has_value());
    EXPECT_EQ(schema.attributes().size(), 2);
    EXPECT_EQ(schema.attributes().at("softDeletesField"), "y");
    EXPECT_EQ(schema.attributes().at("other"), "z");
}

TEST(FieldSchemaTest, AssertSameSchemaAcceptsMatchingFieldInfo) {
    FieldSchema schema("title");
    schema.reset(1);
    ASSERT_TRUE(schema.set_index_options(IndexOptions::DocsAndFreqsAndPositions, false, false).has_value());

    const FieldInfo info = text_field_info();
    EXPECT_TRUE(schema.assert_same_schema(info).has_value());
}

TEST(FieldSchemaTest, AssertSameSchemaRejectsMismatches) {
    FieldSchema schema("title");
    schema.reset(1);
    ASSERT_TRUE(schema.set_index_options(IndexOptions::DocsAndFreqsAndPositions, false, false).has_value());

    FieldInfo info = text_field_info();
    info.index_options = IndexOptions::DocsAndFreqs;
    auto options = schema.assert_same_schema(info);
    ASSERT_FALSE(options.has_value());
    EXPECT_NE(options.error().to_string().find("index options"), std::string::npos);

    FieldInfo stored_norms = text_field_info();
    stored_norms.omit_norms = true;
    EXPECT_FALSE(schema.assert_same_schema(stored_norms).has_value());

    FieldInfo with_vectors = text_field_info();
    with_vectors.store_term_vector = true;
    EXPECT_FALSE(schema.assert_same_schema(with_vectors).has_value());

    FieldInfo with_doc_values = text_field_info();
    with_doc_values.doc_values_type = DocValuesType::Numeric;
    EXPECT_FALSE(schema.assert_same_schema(with_doc_values).has_value());

    FieldInfo with_points = text_field_info();
    with_points.point_dimension_count = 1;
    with_points.point_index_dimension_count = 1;
    with_points.point_num_bytes = 8;
    auto points = schema.assert_same_schema(with_points);
    ASSERT_FALSE(points.has_value());
    EXPECT_NE(points.error().to_string().find("point dimension"), std::string::npos);
}
