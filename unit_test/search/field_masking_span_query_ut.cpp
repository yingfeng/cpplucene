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
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

module lucene_core:ut.field_masking_span_query;

import lucene_core;

using namespace lucene;

namespace {

struct Person {
    std::string first;
    std::string last;
};

std::unique_ptr<Directory> build_person_index(const std::filesystem::path &path, const std::vector<Person> &people) {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);

    auto directory = FSDirectory::open(path);
    EXPECT_TRUE(directory.has_value());
    if (!directory.has_value()) {
        return nullptr;
    }

    FieldType text_type;
    EXPECT_TRUE(text_type.set_index_options(IndexOptions::DocsAndFreqsAndPositions).has_value());

    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(*directory.value(), config);
    EXPECT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());
    if (!writer.has_value()) {
        return nullptr;
    }
    for (const Person &person : people) {
        Document doc;
        doc.add(std::make_unique<Field>("first", FieldValue{person.first}, text_type));
        doc.add(std::make_unique<Field>("last", FieldValue{person.last}, text_type));
        EXPECT_TRUE(writer.value()->add_document(std::move(doc)).has_value());
    }
    EXPECT_TRUE(writer.value()->commit().has_value());
    EXPECT_TRUE(writer.value()->close().has_value());
    return std::move(directory.value());
}

std::shared_ptr<SpanQuery> masked_term(std::string_view field, std::string_view text, std::string_view mask) {
    return std::make_shared<FieldMaskingSpanQuery>(
            std::make_shared<SpanTermQuery>(Term::from_text(std::string(field), text)), std::string(mask));
}

std::shared_ptr<SpanQuery> plain_term(std::string_view field, std::string_view text) {
    return std::make_shared<SpanTermQuery>(Term::from_text(std::string(field), text));
}

} // namespace

TEST(FieldMaskingSpanQueryTest, CombinesClausesFromDifferentFields) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_span_field_masking";
    auto directory = build_person_index(path, {{"james", "jones"}, {"james", "smith"}});
    ASSERT_NE(directory, nullptr);

    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    IndexSearcher searcher(reader.value());

    // `last:jones` is presented under the `first` field, so the two clauses can be
    // combined by a near query.  Both terms sit at position 0, hence the `-1`
    // slop (the two positions overlap by one).
    auto near = SpanNearQuery::create({plain_term("first", "james"), masked_term("last", "jones", "first")}, -1, false);
    ASSERT_TRUE(near.has_value()) << (near.has_value() ? "" : near.error().to_string());
    auto count = searcher.count(near.value());
    ASSERT_TRUE(count.has_value()) << (count.has_value() ? "" : count.error().to_string());
    EXPECT_EQ(count.value(), 1);

    // Without the mask the near query cannot even be built: the fields differ.
    EXPECT_FALSE(SpanNearQuery::create({plain_term("first", "james"), plain_term("last", "jones")}, -1, false)
                         .has_value());

    // The ordered variant cannot see them: it advances the second clause past the
    // first one's end, and both sit at position 0.  That is exactly why the
    // reference implementation's example uses `inOrder = false`.
    auto ordered =
            SpanNearQuery::create({plain_term("first", "james"), masked_term("last", "jones", "first")}, -1, true);
    ASSERT_TRUE(ordered.has_value());
    auto ordered_count = searcher.count(ordered.value());
    ASSERT_TRUE(ordered_count.has_value());
    EXPECT_EQ(ordered_count.value(), 0);

    EXPECT_TRUE(reader.value()->close().has_value());
}

TEST(FieldMaskingSpanQueryTest, PresentsTheSuppliedField) {
    FieldMaskingSpanQuery query(std::make_shared<SpanTermQuery>(Term::from_text("last", "jones")), "first");

    EXPECT_EQ(query.get_field(), "first");
    EXPECT_EQ(query.masked_query()->get_field(), "last");

    // The wrapped query is rendered with the field passed in, so it keeps its own
    // name when that differs from the mask.
    auto text = query.to_string("first");
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text.value(), "mask(SpanTermQuery(last:jones)) as first");

    FieldMaskingSpanQuery same(std::make_shared<SpanTermQuery>(Term::from_text("last", "jones")), "first");
    EXPECT_TRUE(query.equals(same));
    EXPECT_EQ(query.hash(), same.hash());

    FieldMaskingSpanQuery other_mask(std::make_shared<SpanTermQuery>(Term::from_text("last", "jones")), "nickname");
    EXPECT_FALSE(query.equals(other_mask));
}

TEST(FieldMaskingSpanQueryTest, WorksInsideAConjunctionAcrossFields) {
    const auto path = std::filesystem::temp_directory_path() / "lucene_core_span_field_masking_or";
    auto directory = build_person_index(path, {{"james", "jones"}, {"sally", "jones"}, {"james", "smith"}});
    ASSERT_NE(directory, nullptr);

    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    // `first:james` and `last:jones` as two independent clauses of a span or.
    auto or_query = SpanOrQuery::create(
            {plain_term("first", "james"), masked_term("last", "jones", "first")});
    ASSERT_TRUE(or_query.has_value()) << (or_query.has_value() ? "" : or_query.error().to_string());
    auto count = searcher.count(or_query.value());
    ASSERT_TRUE(count.has_value()) << (count.has_value() ? "" : count.error().to_string());
    EXPECT_EQ(count.value(), 3);

    EXPECT_TRUE(reader.value()->close().has_value());
}
