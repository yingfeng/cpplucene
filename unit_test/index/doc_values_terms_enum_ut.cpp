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
#include <expected>
#include <filesystem>
#include <span>
#include <memory>
#include <string>
#include <vector>

module lucene_core:ut.doc_values_terms_enum;

import lucene_core;

using namespace lucene;

namespace {

/// Builds a three document index.
///
/// * `tag` is single valued: `a`, `c`, `d`.
/// * `tags` is multi valued: `{a, b}`, `{b, c}`, `{}`.
/// * `single` is a sorted set with one value per document: `x`, `y`, `z`, which
///   the codec stores as a singleton.
std::shared_ptr<Directory> build_index(std::string_view name) {
    const auto path = std::filesystem::temp_directory_path() / std::string(name);
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);

    auto directory = FSDirectory::open(path);
    EXPECT_TRUE(directory.has_value());
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(*directory.value(), config);
    EXPECT_TRUE(writer.has_value());

    const std::vector<std::string> tags{"a", "c", "d"};
    const std::vector<std::vector<std::string>> tag_sets{{"a", "b"}, {"b", "c"}, {}};
    const std::vector<std::string> singles{"x", "y", "z"};
    for (size_t i = 0; i < tags.size(); ++i) {
        Document doc;
        auto tag = SortedDocValuesField::create("tag", tags[i]);
        EXPECT_TRUE(tag.has_value());
        doc.add(std::make_unique<Field>(std::move(tag.value())));
        for (const std::string &value : tag_sets[i]) {
            auto set = SortedSetDocValuesField::create("tags", value);
            EXPECT_TRUE(set.has_value());
            doc.add(std::make_unique<Field>(std::move(set.value())));
        }
        auto single = SortedSetDocValuesField::create("single", singles[i]);
        EXPECT_TRUE(single.has_value());
        doc.add(std::make_unique<Field>(std::move(single.value())));
        EXPECT_TRUE(writer.value()->add_document(doc).has_value());
    }
    EXPECT_TRUE(writer.value()->commit().has_value());
    EXPECT_TRUE(writer.value()->close().has_value());
    return std::move(directory.value());
}

/// Drains a terms enum into `{"term@ord", ...}`.
std::vector<std::string> drain(TermsEnum &terms) {
    std::vector<std::string> out;
    while (true) {
        auto term = terms.next();
        EXPECT_TRUE(term.has_value()) << (term ? "" : term.error().to_string());
        if (!term.has_value() || term.value() == nullptr) {
            break;
        }
        auto text = term.value()->utf8_to_string();
        EXPECT_TRUE(text.has_value());
        auto ord = terms.ord();
        EXPECT_TRUE(ord.has_value());
        out.push_back((text.has_value() ? text.value() : std::string{}) + "@" +
                      (ord.has_value() ? std::to_string(ord.value()) : std::string{}));
    }
    return out;
}

std::span<const uint8_t> bytes(std::string_view text) {
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

} // namespace

TEST(SortedDocValuesTermsEnumTest, EnumeratesTheDictionaryInOrdinalOrder) {
    auto directory = build_index("lucene_core_dv_terms_enum_sorted");
    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    // `leaves()` returns the `Result` by value, so the span has to be copied: a
    // reference into the temporary would dangle as soon as the full expression
    // ends (Release builds reuse that stack slot immediately).
    const std::span<const LeafReaderContext> leaves = reader.value()->leaves().value();
    ASSERT_EQ(leaves.size(), 1U);

    auto values = leaves[0].reader()->get_sorted_doc_values("tag");
    ASSERT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
    ASSERT_NE(values.value(), nullptr);
    EXPECT_EQ(values.value()->get_value_count().value(), 3);

    auto terms = values.value()->terms_enum();
    ASSERT_TRUE(terms.has_value()) << (terms ? "" : terms.error().to_string());
    EXPECT_EQ(drain(*terms.value()), (std::vector<std::string>{"a@0", "c@1", "d@2"}));

    // `term()` answers the term of the current ordinal after the iteration too.
    auto term = terms.value()->term();
    ASSERT_TRUE(term.has_value());
    EXPECT_EQ(term.value()->utf8_to_string().value(), "d");
}

TEST(SortedDocValuesTermsEnumTest, SeeksExactlyAndByCeil) {
    auto directory = build_index("lucene_core_dv_terms_enum_seek");
    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value());
    auto values = reader.value()->leaves().value()[0].reader()->get_sorted_doc_values("tag");
    ASSERT_TRUE(values.has_value());
    auto terms = values.value()->terms_enum();
    ASSERT_TRUE(terms.has_value());
    TermsEnum &enum_ref = *terms.value();

    ASSERT_TRUE(enum_ref.seek_exact(bytes("c")).value());
    EXPECT_EQ(enum_ref.ord().value(), 1);
    EXPECT_EQ(enum_ref.term().value()->utf8_to_string().value(), "c");

    // A miss leaves the position alone, matching the reference implementation.
    EXPECT_FALSE(enum_ref.seek_exact(bytes("b")).value());
    EXPECT_EQ(enum_ref.ord().value(), 1);

    // A ceil miss lands on the next term; past the end reports End.
    EXPECT_EQ(enum_ref.seek_ceil(bytes("bb")).value(), SeekStatus::NotFound);
    EXPECT_EQ(enum_ref.term().value()->utf8_to_string().value(), "c");
    EXPECT_EQ(enum_ref.seek_ceil(bytes("d")).value(), SeekStatus::Found);
    EXPECT_EQ(enum_ref.ord().value(), 2);
    EXPECT_EQ(enum_ref.seek_ceil(bytes("z")).value(), SeekStatus::End);

    // Ord based positioning, plus the term state it produces.
    ASSERT_TRUE(enum_ref.seek_exact_with_ord(0).has_value());
    EXPECT_EQ(enum_ref.ord().value(), 0);
    EXPECT_EQ(enum_ref.term().value()->utf8_to_string().value(), "a");
    auto state = enum_ref.term_state();
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state.value().ord().value(), 0);
    EXPECT_FALSE(enum_ref.seek_exact_with_ord(3).has_value());

    // Doc values have no postings to return.
    EXPECT_TRUE(enum_ref.postings_with_flags(nullptr, 0).has_value());
    EXPECT_FALSE(enum_ref.impacts(0).has_value());
    EXPECT_FALSE(enum_ref.doc_freq().has_value());
    EXPECT_FALSE(enum_ref.total_term_freq().has_value());
    EXPECT_FALSE(enum_ref.attributes().has_value());

    // Re-iterating after a seek continues from the current ordinal.
    EXPECT_EQ(drain(enum_ref), (std::vector<std::string>{"c@1", "d@2"}));
}

TEST(SortedSetDocValuesTermsEnumTest, EnumeratesMultiValuedDictionaries) {
    auto directory = build_index("lucene_core_dv_terms_enum_set");
    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value());
    auto values = reader.value()->leaves().value()[0].reader()->get_sorted_set_doc_values("tags");
    ASSERT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
    ASSERT_NE(values.value(), nullptr);

    auto terms = values.value()->terms_enum();
    ASSERT_TRUE(terms.has_value()) << (terms ? "" : terms.error().to_string());
    EXPECT_EQ(drain(*terms.value()), (std::vector<std::string>{"a@0", "b@1", "c@2"}));

    ASSERT_TRUE(terms.value()->seek_exact(bytes("b")).value());
    EXPECT_EQ(terms.value()->ord().value(), 1);
    EXPECT_EQ(terms.value()->seek_ceil(bytes("bb")).value(), SeekStatus::NotFound);
    EXPECT_EQ(terms.value()->term().value()->utf8_to_string().value(), "c");
}

TEST(SortedSetDocValuesTermsEnumTest, EnumeratesSingletonFields) {
    auto directory = build_index("lucene_core_dv_terms_enum_singleton");
    auto reader = DirectoryReader::open(*directory);
    ASSERT_TRUE(reader.has_value());
    auto values = reader.value()->leaves().value()[0].reader()->get_sorted_set_doc_values("single");
    ASSERT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
    ASSERT_NE(values.value(), nullptr);
    // One value per document: the codec stores it as a singleton sorted set.
    ASSERT_EQ(values.value()->get_value_count().value(), 3);

    auto terms = values.value()->terms_enum();
    ASSERT_TRUE(terms.has_value()) << (terms ? "" : terms.error().to_string());
    EXPECT_EQ(drain(*terms.value()), (std::vector<std::string>{"x@0", "y@1", "z@2"}));
}
