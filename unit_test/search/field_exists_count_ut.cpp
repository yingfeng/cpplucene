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

// The count shortcuts of `FieldExistsQuery`: dense norms, the point count and the
// term count answer the count without visiting the documents, and the leaves that
// cannot be shortcut report "unknown" so that the searcher falls back to scoring.

module;

#include <gtest/gtest.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.field_exists_count;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kDenseText = "dense_text";
constexpr std::string_view kSparseText = "sparse_text";
constexpr std::string_view kPoint = "p";
constexpr std::string_view kTerms = "terms_field";
constexpr std::string_view kDocValuesOnly = "dv_only";
constexpr std::string_view kStoredOnly = "stored_only";
constexpr std::string_view kId = "id";

/// An indexed text field whose norms are stored unless `omit_norms`.
FieldType text_type(bool omit_norms = false) {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    if (omit_norms) {
        EXPECT_TRUE(type.set_omit_norms(true).has_value());
    }
    return type;
}

/// Builds six documents with one segment; `deleted` removes the first document
/// whose `id` is that value.
void build_index(Directory &directory, const std::optional<std::string> &deleted = std::nullopt) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer.has_value() ? "" : writer.error().to_string());

    for (int32_t doc = 0; doc < 6; ++doc) {
        Document document;
        // Norms on every document, so the dense path can answer the count.
        document.add(std::make_unique<Field>(std::string(kDenseText), FieldValue{std::string("x")}, text_type()));
        document.add(std::make_unique<Field>(std::string(kId), FieldValue{std::to_string(doc)}, text_type()));
        // Norms on the even documents only.
        if (doc % 2 == 0) {
            document.add(std::make_unique<Field>(std::string(kSparseText), FieldValue{std::string("x")}, text_type()));
        }
        // A point plus doc values on the first four documents: the query needs doc
        // values to accept the field, and the count then comes from the points.
        if (doc < 4) {
            auto point = IntPoint::create(std::string(kPoint), std::vector<int32_t>{doc});
            ASSERT_TRUE(point.has_value()) << point.error().to_string();
            document.add(std::make_unique<Field>(std::move(point.value())));
            auto point_values = NumericDocValuesField::create(std::string(kPoint), doc);
            ASSERT_TRUE(point_values.has_value());
            document.add(std::make_unique<Field>(std::move(point_values.value())));
        }
        // Postings without norms plus sorted-set doc values on the first five
        // documents: the count comes from the terms.
        if (doc < 5) {
            auto keyword = KeywordField::create(std::string(kTerms), "x", false);
            ASSERT_TRUE(keyword.has_value()) << keyword.error().to_string();
            document.add(std::make_unique<Field>(std::move(keyword.value())));
            auto only = NumericDocValuesField::create(std::string(kDocValuesOnly), doc);
            ASSERT_TRUE(only.has_value());
            document.add(std::make_unique<Field>(std::move(only.value())));
        }
        // Stored only: the query has nothing to look at and says so.
        document.add(std::make_unique<Field>(StoredField::from_string(std::string(kStoredOnly), "value")));
        ASSERT_TRUE(writer.value()->add_document(std::move(document)).has_value());
    }
    if (deleted.has_value()) {
        ASSERT_TRUE(writer.value()->delete_documents(Term::from_text(std::string(kId), deleted.value())).has_value());
    }
    ASSERT_TRUE(writer.value()->close().has_value());
}

/// The answer of the weight's `count` shortcut for the only leaf of `searcher`:
/// the number of matches, or `-1` when the leaf cannot answer it.
Result<int32_t> shortcut_count(IndexSearcher &searcher, const std::string &field) {
    const FieldExistsQuery query(field);
    auto weight = query.create_weight(searcher, ScoreMode::Complete, 1.0F);
    if (!weight.has_value()) {
        return std::unexpected(std::move(weight.error()));
    }
    const auto leaves = searcher.leaves();
    if (!leaves.has_value()) {
        return std::unexpected(std::move(leaves.error()));
    }
    return weight.value()->count(leaves.value().front(), searcher);
}

} // namespace

TEST(FieldExistsCountTest, ADenseNormFieldIsCountedFromTheLiveDocuments) {
    ByteBuffersDirectory directory;
    build_index(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    IndexSearcher searcher(reader.value());

    auto counted = shortcut_count(searcher, std::string(kDenseText));
    ASSERT_TRUE(counted.has_value()) << counted.error().to_string();
    EXPECT_EQ(counted.value(), 6);

    auto total = searcher.count(FieldExistsQuery(std::string(kDenseText)));
    ASSERT_TRUE(total.has_value()) << total.error().to_string();
    EXPECT_EQ(total.value(), 6);
}

TEST(FieldExistsCountTest, ASparseNormFieldFallsBackToScoring) {
    ByteBuffersDirectory directory;
    build_index(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    // Norms do not cover every document, so the leaf cannot answer the count.
    auto counted = shortcut_count(searcher, std::string(kSparseText));
    ASSERT_TRUE(counted.has_value()) << counted.error().to_string();
    EXPECT_EQ(counted.value(), -1);

    auto total = searcher.count(FieldExistsQuery(std::string(kSparseText)));
    ASSERT_TRUE(total.has_value()) << total.error().to_string();
    EXPECT_EQ(total.value(), 3);
}

TEST(FieldExistsCountTest, APointFieldIsCountedFromItsValues) {
    ByteBuffersDirectory directory;
    build_index(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    auto counted = shortcut_count(searcher, std::string(kPoint));
    ASSERT_TRUE(counted.has_value()) << counted.error().to_string();
    EXPECT_EQ(counted.value(), 4);

    auto total = searcher.count(FieldExistsQuery(std::string(kPoint)));
    ASSERT_TRUE(total.has_value()) << total.error().to_string();
    EXPECT_EQ(total.value(), 4);
}

TEST(FieldExistsCountTest, APostingsFieldWithDocValuesIsCountedFromTheTerms) {
    ByteBuffersDirectory directory;
    build_index(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    auto counted = shortcut_count(searcher, std::string(kTerms));
    ASSERT_TRUE(counted.has_value()) << counted.error().to_string();
    EXPECT_EQ(counted.value(), 5);

    auto total = searcher.count(FieldExistsQuery(std::string(kTerms)));
    ASSERT_TRUE(total.has_value()) << total.error().to_string();
    EXPECT_EQ(total.value(), 5);
}

TEST(FieldExistsCountTest, ADocValuesOnlyFieldFallsBackToScoring) {
    ByteBuffersDirectory directory;
    build_index(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    // No point and no postings to read a count from.
    auto counted = shortcut_count(searcher, std::string(kDocValuesOnly));
    ASSERT_TRUE(counted.has_value()) << counted.error().to_string();
    EXPECT_EQ(counted.value(), -1);

    auto total = searcher.count(FieldExistsQuery(std::string(kDocValuesOnly)));
    ASSERT_TRUE(total.has_value()) << total.error().to_string();
    EXPECT_EQ(total.value(), 5);
}

TEST(FieldExistsCountTest, AMissingFieldCountsNothing) {
    ByteBuffersDirectory directory;
    build_index(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    auto counted = shortcut_count(searcher, "nope");
    ASSERT_TRUE(counted.has_value()) << counted.error().to_string();
    EXPECT_EQ(counted.value(), 0);
    auto total = searcher.count(FieldExistsQuery("nope"));
    ASSERT_TRUE(total.has_value());
    EXPECT_EQ(total.value(), 0);
}

TEST(FieldExistsCountTest, AFieldWithNoValuesIsRejected) {
    ByteBuffersDirectory directory;
    build_index(directory);
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    IndexSearcher searcher(reader.value());

    auto counted = shortcut_count(searcher, std::string(kStoredOnly));
    ASSERT_FALSE(counted.has_value());
    EXPECT_EQ(counted.error().kind(), ErrorKind::kIllegalArgument);
    auto total = searcher.count(FieldExistsQuery(std::string(kStoredOnly)));
    ASSERT_FALSE(total.has_value());
    EXPECT_EQ(total.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(FieldExistsCountTest, DeletionsDisableThePointAndTermShortcuts) {
    ByteBuffersDirectory directory;
    build_index(directory, std::string("0")); // the first document has the point and the terms
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value());
    ASSERT_TRUE(reader.value()->has_deletions().value());
    IndexSearcher searcher(reader.value());

    auto point = shortcut_count(searcher, std::string(kPoint));
    ASSERT_TRUE(point.has_value()) << point.error().to_string();
    EXPECT_EQ(point.value(), -1);
    auto point_total = searcher.count(FieldExistsQuery(std::string(kPoint)));
    ASSERT_TRUE(point_total.has_value()) << point_total.error().to_string();
    EXPECT_EQ(point_total.value(), 3);

    auto terms = shortcut_count(searcher, std::string(kTerms));
    ASSERT_TRUE(terms.has_value()) << terms.error().to_string();
    EXPECT_EQ(terms.value(), -1);
    auto terms_total = searcher.count(FieldExistsQuery(std::string(kTerms)));
    ASSERT_TRUE(terms_total.has_value()) << terms_total.error().to_string();
    EXPECT_EQ(terms_total.value(), 4);

    // Dense norms still cover every document, but the count has to exclude the
    // deleted one, so the shortcut is declined as well.
    auto dense = shortcut_count(searcher, std::string(kDenseText));
    ASSERT_TRUE(dense.has_value()) << dense.error().to_string();
    EXPECT_EQ(dense.value(), 5);
    auto dense_total = searcher.count(FieldExistsQuery(std::string(kDenseText)));
    ASSERT_TRUE(dense_total.has_value());
    EXPECT_EQ(dense_total.value(), 5);
}
