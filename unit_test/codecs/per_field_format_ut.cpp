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

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.per_field_format;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kIdField = "id";
constexpr std::string_view kTagField = "tag";
constexpr std::string_view kNumField = "num";

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqsAndPositions).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

FieldType numeric_type() {
    FieldType type;
    EXPECT_TRUE(type.set_doc_values_type(DocValuesType::Numeric).has_value());
    return type;
}

/// A document with a stored text field, a sorted doc values field and a numeric
/// doc values field, so both per-field wrappers have something to write.
Document make_doc(const std::string &id, int64_t num) {
    Document doc;
    doc.add(std::make_unique<Field>(std::string(kIdField), FieldValue{id}, text_type()));
    auto tag = SortedDocValuesField::create(std::string(kTagField), id);
    EXPECT_TRUE(tag.has_value()) << (tag ? "" : tag.error().to_string());
    if (tag.has_value()) {
        doc.add(std::make_unique<Field>(std::move(tag.value())));
    }
    auto numeric = Field::from_i64(std::string(kNumField), num, numeric_type());
    EXPECT_TRUE(numeric.has_value()) << (numeric ? "" : numeric.error().to_string());
    if (numeric.has_value()) {
        doc.add(std::make_unique<Field>(std::move(numeric.value())));
    }
    return doc;
}

std::vector<std::string> file_names(const Directory &dir) {
    auto listed = dir.list_all();
    EXPECT_TRUE(listed.has_value()) << (listed ? "" : listed.error().to_string());
    return listed.has_value() ? listed.value() : std::vector<std::string>{};
}

bool any_ends_with(const std::vector<std::string> &names, std::string_view suffix) {
    return std::ranges::any_of(names, [suffix](const std::string &name) { return name.ends_with(suffix); });
}

bool contains(const std::vector<std::string> &names, std::string_view file) {
    return std::ranges::find(names, file) != names.end();
}

std::vector<int64_t> numeric_values(LeafReader &leaf) {
    std::vector<int64_t> values;
    auto cursor = leaf.get_numeric_doc_values(std::string(kNumField));
    EXPECT_TRUE(cursor.has_value()) << (cursor ? "" : cursor.error().to_string());
    if (!cursor.has_value() || cursor.value() == nullptr) {
        return values;
    }
    while (true) {
        auto doc = cursor.value()->next_doc();
        EXPECT_TRUE(doc.has_value());
        if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
            break;
        }
        auto value = cursor.value()->long_value();
        EXPECT_TRUE(value.has_value());
        if (value.has_value()) {
            values.push_back(value.value());
        }
    }
    return values;
}

} // namespace

TEST(PerFieldFormatTest, WritesCodecSuffixedFiles) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    // The test inspects the files of a segment, so it must not be packed.
    config.set_use_compound_file(false);
    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    ASSERT_TRUE(writer.value()->add_document(make_doc("a", 1)).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("b", 2)).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    const auto names = file_names(dir);
    // Postings and doc values files carry `<format>_<n>` after the segment name,
    // like a segment written by the reference implementation.
    EXPECT_TRUE(any_ends_with(names, "Lucene101_0.tim")) << names.size() << " files";
    EXPECT_TRUE(any_ends_with(names, "Lucene101_0.tmd"));
    EXPECT_TRUE(any_ends_with(names, "Lucene90_0.dvd"));
    EXPECT_TRUE(any_ends_with(names, "Lucene90_0.dvm"));
    // Norms, points, stored fields and term vectors are not per-field.
    EXPECT_TRUE(any_ends_with(names, ".nvd"));
    EXPECT_TRUE(any_ends_with(names, ".fdt"));
    // No segment file without a suffix is left behind.
    EXPECT_FALSE(contains(names, "_0.tim"));
    EXPECT_FALSE(contains(names, "_0.dvd"));
    EXPECT_TRUE(contains(names, "_0_Lucene101_0.tim"));
    EXPECT_TRUE(contains(names, "_0_Lucene90_0.dvd"));
}

TEST(PerFieldFormatTest, RecordsTheFormatOfEveryField) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    ASSERT_TRUE(writer.value()->add_document(make_doc("a", 1)).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto reader = DirectoryReader::open(dir);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    auto leaves = reader.value()->leaves();
    ASSERT_TRUE(leaves.has_value());
    ASSERT_EQ(leaves.value().size(), 1U);
    LeafReader &leaf = *leaves.value()[0].reader();

    auto infos = leaf.field_infos();
    ASSERT_TRUE(infos.has_value());
    const FieldInfo *text = infos.value()->by_name(kIdField);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->attributes.at(std::string(PER_FIELD_POSTINGS_FORMAT_KEY)), "Lucene101");
    EXPECT_EQ(text->attributes.at(std::string(PER_FIELD_POSTINGS_SUFFIX_KEY)), "0");
    EXPECT_EQ(text->attributes.count(std::string(PER_FIELD_DV_FORMAT_KEY)), 0U);

    const FieldInfo *numeric = infos.value()->by_name(kNumField);
    ASSERT_NE(numeric, nullptr);
    EXPECT_EQ(numeric->attributes.at(std::string(PER_FIELD_DV_FORMAT_KEY)), "Lucene90");
    EXPECT_EQ(numeric->attributes.at(std::string(PER_FIELD_DV_SUFFIX_KEY)), "0");
    EXPECT_EQ(numeric->attributes.count(std::string(PER_FIELD_POSTINGS_FORMAT_KEY)), 0U);
}

TEST(PerFieldFormatTest, ReadsBackThroughTheWrappers) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    ASSERT_TRUE(writer.value()->add_document(make_doc("a", 10)).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("b", 20)).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto reader = DirectoryReader::open(dir);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    ASSERT_EQ(reader.value()->num_docs().value(), 2);

    // Postings still resolve through the wrapper.
    IndexSearcher searcher(reader.value());
    auto hits = searcher.search(TermQuery(Term("id", BytesRef::from_string("b"))), 10);
    ASSERT_TRUE(hits.has_value()) << (hits ? "" : hits.error().to_string());
    ASSERT_EQ(hits.value().score_docs.size(), 1U);

    // Doc values too.
    auto leaves = reader.value()->leaves();
    ASSERT_TRUE(leaves.has_value());
    auto values = numeric_values(*leaves.value()[0].reader());
    EXPECT_EQ(values, (std::vector<int64_t>{10, 20}));
}

TEST(PerFieldFormatTest, MergingKeepsSuffixesAndAttributes) {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_max_buffered_docs(1);
    auto writer = IndexWriter::open(dir, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    ASSERT_TRUE(writer.value()->add_document(make_doc("a", 1)).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("b", 2)).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    // Two documents buffered one at a time produce two segments.
    ASSERT_EQ(writer.value()->segment_infos().segments.size(), 2U);

    ASSERT_TRUE(writer.value()->force_merge(1).has_value());
    ASSERT_EQ(writer.value()->segment_infos().segments.size(), 1U);

    auto reader = DirectoryReader::open(dir);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    auto leaves = reader.value()->leaves();
    ASSERT_TRUE(leaves.has_value());
    ASSERT_EQ(leaves.value().size(), 1U);
    LeafReader &leaf = *leaves.value()[0].reader();

    // The merged segment keeps the per-field attributes, so the wrapper can find
    // the files it wrote.
    auto infos = leaf.field_infos();
    ASSERT_TRUE(infos.has_value());
    EXPECT_EQ(infos.value()->by_name(kIdField)->attributes.at(std::string(PER_FIELD_POSTINGS_FORMAT_KEY)),
              "Lucene101");
    EXPECT_EQ(infos.value()->by_name(kNumField)->attributes.at(std::string(PER_FIELD_DV_FORMAT_KEY)), "Lucene90");

    EXPECT_EQ(leaf.num_docs().value(), 2);
    EXPECT_EQ(numeric_values(leaf), (std::vector<int64_t>{1, 2}));

    IndexSearcher searcher(reader.value());
    auto hits = searcher.search(TermQuery(Term("id", BytesRef::from_string("a"))), 10);
    ASSERT_TRUE(hits.has_value()) << (hits ? "" : hits.error().to_string());
    EXPECT_EQ(hits.value().score_docs.size(), 1U);
}

TEST(FieldInfosFilterTest, FiltersTheIterationViewOnly) {
    FieldInfo a;
    a.name = "a";
    a.number = 0;
    a.index_options = IndexOptions::DocsAndFreqs;
    FieldInfo b;
    b.name = "b";
    b.number = 1;
    b.doc_values_type = DocValuesType::Numeric;
    FieldInfo c;
    c.name = "c";
    c.number = 2;
    c.point_dimension_count = 1;
    c.point_index_dimension_count = 1;
    c.point_num_bytes = 8;

    auto infos = FieldInfos::of({a, b, c});
    ASSERT_TRUE(infos.has_value()) << (infos ? "" : infos.error().to_string());

    const FieldInfos filtered = infos.value().with_filter({"b"});
    ASSERT_EQ(filtered.fields().size(), 1U);
    EXPECT_EQ(filtered.fields()[0].name, "b");
    // The predicates follow the view ...
    EXPECT_TRUE(filtered.has_doc_values());
    EXPECT_FALSE(filtered.has_postings());
    EXPECT_FALSE(filtered.has_point_values());
    EXPECT_FALSE(filtered.has_norms());
    // ... while name and number lookups still see everything, so field numbering
    // survives a merge.
    EXPECT_EQ(filtered.size(), 3U);
    ASSERT_NE(filtered.by_name("a"), nullptr);
    EXPECT_EQ(filtered.by_name("a")->number, 0);
    ASSERT_NE(filtered.by_number(2), nullptr);
    EXPECT_EQ(filtered.by_number(2)->name, "c");

    const FieldInfos none = infos.value().with_filter({});
    EXPECT_TRUE(none.fields().empty());
    EXPECT_FALSE(none.has_postings());
    EXPECT_FALSE(none.has_doc_values());

    // The original is untouched.
    EXPECT_EQ(infos.value().fields().size(), 3U);
    EXPECT_TRUE(infos.value().has_postings());
}
