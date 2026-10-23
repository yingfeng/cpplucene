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
#include <optional>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.index_sorter;

import lucene_core;

using namespace lucene;

namespace {

/// A leaf reader over the documents a test wrote, plus the directory that owns
/// the files.
struct Fixture {
    std::shared_ptr<ByteBuffersDirectory> directory = std::make_shared<ByteBuffersDirectory>();
    std::shared_ptr<DirectoryReader> reader;
    LeafReader *leaf{nullptr};

    explicit Fixture(std::vector<std::function<void(Document &, int32_t)>> adders, int32_t count) {
        WhitespaceAnalyzer analyzer;
        auto writer = IndexWriter::open(*directory, IndexWriterConfig(analyzer));
        EXPECT_TRUE(writer.has_value());
        if (!writer.has_value()) {
            return;
        }
        for (int32_t doc_id = 0; doc_id < count; ++doc_id) {
            Document doc;
            for (const auto &add : adders) {
                add(doc, doc_id);
            }
            auto added = writer.value()->add_document(doc);
            EXPECT_TRUE(added.has_value()) << (added ? "" : added.error().to_string());
        }
        auto committed = writer.value()->commit();
        EXPECT_TRUE(committed.has_value()) << (committed ? "" : committed.error().to_string());
        EXPECT_TRUE(writer.value()->close().has_value());

        auto opened = DirectoryReader::open(*directory);
        EXPECT_TRUE(opened.has_value()) << (opened ? "" : opened.error().to_string());
        if (!opened.has_value()) {
            return;
        }
        reader = opened.value();
        auto leaves = reader->leaves();
        EXPECT_TRUE(leaves.has_value());
        if (!leaves.has_value() || leaves.value().empty()) {
            return;
        }
        leaf = leaves.value().front().reader();
    }

    [[nodiscard]] int32_t max_doc() const {
        return leaf == nullptr ? 0 : leaf->max_doc().value();
    }
};

/// A one-value field adder: `field` gets `value` in document `doc_id`.
template <typename Add>
[[nodiscard]] std::function<void(Document &, int32_t)> per_doc(Add add) {
    return [add](Document &doc, int32_t doc_id) {
        auto field = add(doc_id);
        EXPECT_TRUE(field.has_value()) << (field ? "" : field.error().to_string());
        if (field.has_value()) {
            doc.add(std::make_unique<Field>(std::move(field.value())));
        }
    };
}

} // namespace

TEST(IndexSorterTest, SortsNumericDocValues) {
    const std::vector<int64_t> values{3, 1, 4, 2};
    Fixture fixture({per_doc([&values](int32_t doc_id) {
                        return NumericDocValuesField::create("rank", values[static_cast<size_t>(doc_id)]);
                    })},
                    4);
    ASSERT_NE(fixture.leaf, nullptr);

    auto sorter = IntIndexSorter::create("rank", std::nullopt, false);
    ASSERT_TRUE(sorter.has_value()) << (sorter ? "" : sorter.error().to_string());

    auto map = sort_documents(sorter.value(), *fixture.leaf, fixture.max_doc());
    ASSERT_TRUE(map.has_value()) << (map ? "" : map.error().to_string());
    ASSERT_TRUE(map.value().has_value());

    // 1 -> doc 1, 2 -> doc 3, 3 -> doc 0, 4 -> doc 2.
    EXPECT_EQ(map.value()->new_to_old_values(), (std::vector<int32_t>{1, 3, 0, 2}));
    EXPECT_EQ(map.value()->old_to_new_values(), (std::vector<int32_t>{2, 0, 3, 1}));
    EXPECT_EQ(map.value()->size(), 4);
    EXPECT_EQ(map.value()->old_to_new(0).value(), 2);
    EXPECT_EQ(map.value()->new_to_old(3).value(), 2);
    EXPECT_TRUE(is_index_sort_doc_map_consistent(map.value().value()));

    // Out of range ids are rejected instead of reading past the array.
    EXPECT_FALSE(map.value()->old_to_new(4).has_value());
    EXPECT_FALSE(map.value()->new_to_old(-1).has_value());
}

TEST(IndexSorterTest, ReversesTheOrder) {
    const std::vector<int64_t> values{3, 1, 4, 2};
    Fixture fixture({per_doc([&values](int32_t doc_id) {
                        return NumericDocValuesField::create("rank", values[static_cast<size_t>(doc_id)]);
                    })},
                    4);
    ASSERT_NE(fixture.leaf, nullptr);

    auto sorter = LongIndexSorter::create("rank", std::nullopt, true);
    ASSERT_TRUE(sorter.has_value());
    auto map = sort_documents(sorter.value(), *fixture.leaf, fixture.max_doc());
    ASSERT_TRUE(map.has_value());
    ASSERT_TRUE(map.value().has_value());
    EXPECT_EQ(map.value()->new_to_old_values(), (std::vector<int32_t>{2, 0, 3, 1}));
}

TEST(IndexSorterTest, ReturnsNoMapWhenAlreadySorted) {
    const std::vector<int64_t> values{1, 2, 3, 4};
    Fixture fixture({per_doc([&values](int32_t doc_id) {
                        return NumericDocValuesField::create("rank", values[static_cast<size_t>(doc_id)]);
                    })},
                    4);
    ASSERT_NE(fixture.leaf, nullptr);

    auto sorter = LongIndexSorter::create("rank", std::nullopt, false);
    ASSERT_TRUE(sorter.has_value());
    auto map = sort_documents(sorter.value(), *fixture.leaf, fixture.max_doc());
    ASSERT_TRUE(map.has_value()) << (map ? "" : map.error().to_string());
    EXPECT_FALSE(map.value().has_value());

    // Fewer than two documents are trivially sorted as well.
    Fixture single({per_doc([](int32_t) { return NumericDocValuesField::create("rank", 7); })}, 1);
    ASSERT_NE(single.leaf, nullptr);
    auto single_map = sort_documents(sorter.value(), *single.leaf, single.max_doc());
    ASSERT_TRUE(single_map.has_value());
    EXPECT_FALSE(single_map.value().has_value());
}

TEST(IndexSorterTest, SortsFloatAndDoubleDocValues) {
    const std::vector<double> doubles{2.5, -1.5, 0.0};
    Fixture fixture({per_doc([&doubles](int32_t doc_id) {
                        return DoubleDocValuesField::create("d", doubles[static_cast<size_t>(doc_id)]);
                    })},
                    3);
    ASSERT_NE(fixture.leaf, nullptr);

    auto sorter = DoubleIndexSorter::create("d", std::nullopt, false);
    ASSERT_TRUE(sorter.has_value());
    auto map = sort_documents(sorter.value(), *fixture.leaf, fixture.max_doc());
    ASSERT_TRUE(map.has_value());
    ASSERT_TRUE(map.value().has_value());
    EXPECT_EQ(map.value()->new_to_old_values(), (std::vector<int32_t>{1, 2, 0}));

    const std::vector<float> floats{0.25F, -2.0F, 1.0F};
    Fixture float_fixture({per_doc([&floats](int32_t doc_id) {
                              return FloatDocValuesField::create("f", floats[static_cast<size_t>(doc_id)]);
                          })},
                          3);
    ASSERT_NE(float_fixture.leaf, nullptr);

    auto float_sorter = FloatIndexSorter::create("f", std::nullopt, false);
    ASSERT_TRUE(float_sorter.has_value());
    auto float_map = sort_documents(float_sorter.value(), *float_fixture.leaf, float_fixture.max_doc());
    ASSERT_TRUE(float_map.has_value());
    ASSERT_TRUE(float_map.value().has_value());
    EXPECT_EQ(float_map.value()->new_to_old_values(), (std::vector<int32_t>{1, 0, 2}));
}

TEST(IndexSorterTest, SortsTermDocValuesAndPutsMissingFirst) {
    // Three documents have a term; the fourth has none at all.
    const std::vector<std::string> terms{"banana", "apple", "cherry"};
    std::vector<std::function<void(Document &, int32_t)>> adders;
    adders.push_back([&terms](Document &doc, int32_t doc_id) {
        if (static_cast<size_t>(doc_id) >= terms.size()) {
            return;
        }
        auto field = SortedDocValuesField::create("term", terms[static_cast<size_t>(doc_id)]);
        EXPECT_TRUE(field.has_value()) << (field ? "" : field.error().to_string());
        if (field.has_value()) {
            doc.add(std::make_unique<Field>(std::move(field.value())));
        }
    });
    adders.push_back([&terms](Document &doc, int32_t doc_id) {
        auto field = NumericDocValuesField::create("rank", doc_id);
        EXPECT_TRUE(field.has_value());
        if (field.has_value() && static_cast<size_t>(doc_id) >= terms.size()) {
            doc.add(std::make_unique<Field>(std::move(field.value())));
        }
    });
    Fixture fixture(adders, 4);
    ASSERT_NE(fixture.leaf, nullptr);

    auto sorter = StringIndexSorter::create("term", false, false);
    ASSERT_TRUE(sorter.has_value()) << (sorter ? "" : sorter.error().to_string());
    auto map = sort_documents(sorter.value(), *fixture.leaf, fixture.max_doc());
    ASSERT_TRUE(map.has_value()) << (map ? "" : map.error().to_string());
    ASSERT_TRUE(map.value().has_value());
    // The document without a term sorts first, then apple (1), banana (0), cherry (2).
    EXPECT_EQ(map.value()->new_to_old_values(), (std::vector<int32_t>{3, 1, 0, 2}));
    EXPECT_TRUE(is_index_sort_doc_map_consistent(map.value().value()));

    // Sorting the missing values last puts that document at the end.
    auto missing_last = StringIndexSorter::create("term", true, false);
    ASSERT_TRUE(missing_last.has_value());
    auto last_map = sort_documents(missing_last.value(), *fixture.leaf, fixture.max_doc());
    ASSERT_TRUE(last_map.has_value());
    ASSERT_TRUE(last_map.value().has_value());
    EXPECT_EQ(last_map.value()->new_to_old_values(), (std::vector<int32_t>{1, 0, 2, 3}));
}

TEST(IndexSorterTest, UsesTheMissingValueForDocumentsWithoutTheField) {
    // The first two documents have no `rank` doc values, so they sort as the
    // default missing value of 0; the two that do sort by their (smaller) values.
    Fixture fixture({per_doc([](int32_t doc_id) {
                        if (doc_id < 2) {
                            return NumericDocValuesField::create("other", doc_id);
                        }
                        return NumericDocValuesField::create("rank", doc_id - 3);
                    })},
                    4);
    ASSERT_NE(fixture.leaf, nullptr);

    auto sorter = IntIndexSorter::create("rank", std::nullopt, false);
    ASSERT_TRUE(sorter.has_value());
    auto map = sort_documents(sorter.value(), *fixture.leaf, fixture.max_doc());
    ASSERT_TRUE(map.has_value()) << (map ? "" : map.error().to_string());
    ASSERT_TRUE(map.value().has_value());
    // rank -1 (doc 2) first, then the three zeroes in document order.
    EXPECT_EQ(map.value()->new_to_old_values(), (std::vector<int32_t>{2, 0, 1, 3}));
}

TEST(IndexSorterTest, RejectsInvalidSorters) {
    EXPECT_FALSE(IntIndexSorter::create("", std::nullopt, false).has_value());
    EXPECT_FALSE(LongIndexSorter::create("", std::nullopt, false).has_value());
    EXPECT_FALSE(FloatIndexSorter::create("", std::nullopt, false).has_value());
    EXPECT_FALSE(DoubleIndexSorter::create("", std::nullopt, false).has_value());
    EXPECT_FALSE(StringIndexSorter::create("", false, false).has_value());
}

TEST(IndexSorterTest, DescribesItself) {
    auto numeric = IntIndexSorter::create("rank", 42, false);
    ASSERT_TRUE(numeric.has_value());
    EXPECT_EQ(numeric.value().provider_name(), "int");
    EXPECT_EQ(numeric.value().field(), "rank");
    EXPECT_FALSE(numeric.value().reverse());
    ASSERT_TRUE(numeric.value().missing_value().has_value());
    EXPECT_EQ(number_as_i64(numeric.value().missing_value().value()), 42);
    EXPECT_EQ(numeric.value().to_string(), "int missing=42");

    auto text = StringIndexSorter::create("term", true, true);
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text.value().provider_name(), "string");
    EXPECT_TRUE(text.value().missing_last());
    EXPECT_TRUE(text.value().reverse());
    EXPECT_FALSE(text.value().missing_value().has_value());
}
