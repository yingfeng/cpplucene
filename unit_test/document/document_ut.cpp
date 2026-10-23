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

module lucene_core:ut.document;

import lucene_core;

using namespace lucene;

TEST(FieldTypeTest, DefaultsMatchLucene) {
    FieldType type;
    EXPECT_FALSE(type.stored());
    EXPECT_TRUE(type.tokenized());
    EXPECT_FALSE(type.store_term_vectors());
    EXPECT_FALSE(type.omit_norms());
    EXPECT_EQ(type.index_options(), IndexOptions::None);
    EXPECT_EQ(type.doc_values_type(), DocValuesType::None);
    EXPECT_FALSE(type.frozen());
    EXPECT_EQ(type.to_string(), "");
}

TEST(FieldTypeTest, FreezeBlocksMutation) {
    FieldType type;
    ASSERT_TRUE(type.set_stored(true).has_value());
    type.freeze();
    EXPECT_TRUE(type.frozen());
    auto result = type.set_stored(false);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind(), ErrorKind::kIllegalState);
}

TEST(FieldTypeTest, DimensionValidation) {
    FieldType type;
    EXPECT_TRUE(type.set_dimensions(1, 4).has_value());
    EXPECT_EQ(type.point_dimension_count(), 1u);
    EXPECT_EQ(type.point_index_dimension_count(), 1u);
    EXPECT_EQ(type.point_num_bytes(), 4u);

    // A dimension may hold up to sixteen bytes, which is what an `InetAddressPoint`
    // (a single 128-bit dimension) needs.
    FieldType wide;
    EXPECT_TRUE(wide.set_dimensions(1, 16).has_value());
    EXPECT_EQ(wide.point_num_bytes(), 16u);

    FieldType invalid;
    EXPECT_FALSE(invalid.set_dimensions(0, 0, 4).has_value());
    EXPECT_FALSE(invalid.set_dimensions(1, 0, 4).has_value());
    EXPECT_FALSE(invalid.set_dimensions(17, 17, 4).has_value());
    EXPECT_FALSE(invalid.set_dimensions(1, 1, 17).has_value());
}

TEST(FieldTypeTest, ToStringMatchesLucene) {
    FieldType type;
    ASSERT_TRUE(type.set_stored(true).has_value());
    ASSERT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqsAndPositions).has_value());
    EXPECT_EQ(type.to_string(), "stored,indexed,tokenized");

    FieldType string_type;
    ASSERT_TRUE(string_type.set_index_options(IndexOptions::Docs).has_value());
    ASSERT_TRUE(string_type.set_tokenized(false).has_value());
    ASSERT_TRUE(string_type.set_omit_norms(true).has_value());
    EXPECT_EQ(string_type.to_string(), "indexed,omitNorms,indexOptions=Docs");

    FieldType doc_values;
    ASSERT_TRUE(doc_values.set_doc_values_type(DocValuesType::SortedSet).has_value());
    EXPECT_EQ(doc_values.to_string(), "docValuesType=SortedSet");
}

TEST(FieldTest, FactoriesCarryValues) {
    auto text = TextField::create("title", "hello world", true);
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text->name(), "title");
    EXPECT_TRUE(text->field_type().stored());
    EXPECT_TRUE(text->field_type().tokenized());
    EXPECT_EQ(text->string_value().value(), "hello world");
    EXPECT_FALSE(text->numeric_value().has_value());

    auto number = IntField::create("year", 2026, false);
    ASSERT_TRUE(number.has_value());
    ASSERT_TRUE(number->numeric_value().has_value());
    EXPECT_EQ(number_as_i32(number->numeric_value().value()), 2026);
    EXPECT_EQ(number->field_type().point_dimension_count(), 1u);
    EXPECT_EQ(number->field_type().point_num_bytes(), 4u);
    EXPECT_EQ(number->field_type().doc_values_type(), DocValuesType::Numeric);

    auto keyword = KeywordField::create("tag", "cxx", true);
    ASSERT_TRUE(keyword.has_value());
    EXPECT_EQ(keyword->field_type().doc_values_type(), DocValuesType::SortedSet);
    EXPECT_FALSE(keyword->field_type().tokenized());

    auto stored = StoredField::from_string("raw", "value");
    EXPECT_TRUE(stored.field_type().stored());
    EXPECT_EQ(stored.field_type().index_options(), IndexOptions::None);
    EXPECT_TRUE(stored.field_type().frozen());
}

TEST(DocumentTest, AddGetAndRemove) {
    Document document;
    auto title = TextField::create("title", "first", true);
    auto title2 = TextField::create("title", "second", true);
    auto id = StringField::create("id", "doc-1", false);
    auto body = TextField::create("body", "lorem ipsum", false);
    ASSERT_TRUE(title && title2 && id && body);

    document.add(std::make_unique<Field>(std::move(title.value())));
    document.add(std::make_unique<Field>(std::move(title2.value())));
    document.add(std::make_unique<Field>(std::move(id.value())));
    document.add(std::make_unique<Field>(std::move(body.value())));

    EXPECT_EQ(document.field_count(), 4u);
    ASSERT_NE(document.get_field("title"), nullptr);
    EXPECT_EQ(document.get_values("title").value().size(), 2u);
    EXPECT_EQ(document.get_values("title").value()[0], "first");
    EXPECT_EQ(document.get("id").value().value(), "doc-1");
    EXPECT_FALSE(document.get("missing").value().has_value());

    document.remove_field("title");
    EXPECT_EQ(document.get_values("title").value().size(), 1u);
    EXPECT_EQ(document.get_values("title").value()[0], "second");

    document.remove_fields("title");
    EXPECT_EQ(document.get_field("title"), nullptr);
    EXPECT_EQ(document.field_count(), 2u);

    // Removing a missing field is a no-op.
    document.remove_field("nope");
    EXPECT_EQ(document.field_count(), 2u);

    document.clear();
    EXPECT_TRUE(document.empty());
}

TEST(DocumentTest, StringValueErrorsOnBinaryFields) {
    Document document;
    document.add(std::make_unique<Field>(StoredField::from_bytes("blob", {1, 2, 3})));
    EXPECT_FALSE(document.get("blob").has_value());
    const auto binary = document.get_binary_values("blob");
    ASSERT_TRUE(binary.has_value());
    ASSERT_EQ(binary->size(), 1u);
    EXPECT_EQ((*binary)[0], (std::vector<uint8_t>{1, 2, 3}));
}
