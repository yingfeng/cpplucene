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

module lucene_core:ut.doc_values_fields;

import lucene_core;

using namespace lucene;

namespace {

/// Indexes the documents produced by `build` and reopens the reader.
template <typename Builder>
std::shared_ptr<DirectoryReader> index_documents(int32_t count, Builder build) {
    auto dir = std::make_shared<ByteBuffersDirectory>();
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(*dir, IndexWriterConfig(analyzer));
    EXPECT_TRUE(writer.has_value());
    if (!writer.has_value()) {
        return nullptr;
    }
    for (int32_t doc_id = 0; doc_id < count; ++doc_id) {
        Document doc;
        build(doc, doc_id);
        auto added = writer.value()->add_document(doc);
        EXPECT_TRUE(added.has_value()) << (added ? "" : added.error().to_string());
    }
    EXPECT_TRUE(writer.value()->commit().has_value());
    auto reader = DirectoryReader::open(*dir);
    EXPECT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    return reader.has_value() ? reader.value() : nullptr;
}

std::vector<int32_t> search_docs(const Query &query, std::shared_ptr<DirectoryReader> &reader) {
    IndexSearcher searcher(reader);
    auto top = searcher.search(query, 10);
    EXPECT_TRUE(top.has_value()) << (top ? "" : top.error().to_string());
    std::vector<int32_t> docs;
    if (!top.has_value()) {
        return docs;
    }
    for (const ScoreDoc &hit : top.value().score_docs) {
        docs.push_back(hit.doc);
    }
    std::ranges::sort(docs);
    return docs;
}

} // namespace

TEST(DocValuesFieldsTest, BuildsANumericDocValuesField) {
    auto numeric = NumericDocValuesField::create("n", 42);
    ASSERT_TRUE(numeric.has_value()) << (numeric ? "" : numeric.error().to_string());
    EXPECT_EQ(numeric.value().field_type().doc_values_type(), DocValuesType::Numeric);
    ASSERT_TRUE(numeric.value().numeric_value().has_value());
    EXPECT_EQ(number_as_i64(numeric.value().numeric_value().value()), 42);
    EXPECT_FALSE(numeric.value().field_type().is_indexed());
    EXPECT_FALSE(numeric.value().field_type().stored());

    auto reader = index_documents(2, [](Document &doc, int32_t doc_id) {
        auto field = NumericDocValuesField::create("n", doc_id * 10);
        EXPECT_TRUE(field.has_value());
        doc.add(std::make_unique<Field>(std::move(field.value())));
    });
    ASSERT_NE(reader, nullptr);
    auto leaves = reader->leaves();
    ASSERT_TRUE(leaves.has_value());
    auto values = leaves.value()[0].reader()->get_numeric_doc_values("n");
    ASSERT_TRUE(values.has_value());
    ASSERT_NE(values.value(), nullptr);
    EXPECT_EQ(values.value()->advance_exact(0).value(), true);
    EXPECT_EQ(values.value()->long_value().value(), 0);
    EXPECT_EQ(values.value()->advance_exact(1).value(), true);
    EXPECT_EQ(values.value()->long_value().value(), 10);
}

TEST(DocValuesFieldsTest, BuildsABinaryDocValuesField) {
    auto binary = BinaryDocValuesField::create("b", std::vector<uint8_t>{1, 2, 3});
    ASSERT_TRUE(binary.has_value()) << (binary ? "" : binary.error().to_string());
    EXPECT_EQ(binary.value().field_type().doc_values_type(), DocValuesType::Binary);
    auto bytes = binary.value().binary_value();
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(bytes.value(), (std::vector<uint8_t>{1, 2, 3}));

    // The value can be replaced...
    ASSERT_TRUE(BinaryDocValuesField::set_value(binary.value(), std::vector<uint8_t>{4}).has_value());
    EXPECT_EQ(binary.value().binary_value().value(), (std::vector<uint8_t>{4}));

    // ... but not on another kind of field.
    auto numeric = NumericDocValuesField::create("n", 1);
    ASSERT_TRUE(numeric.has_value());
    auto rejected = BinaryDocValuesField::set_value(numeric.value(), std::vector<uint8_t>{1});
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(DocValuesFieldsTest, BuildsSortedAndSortedSetDocValuesFields) {
    auto sorted = SortedDocValuesField::create("s", "apple");
    ASSERT_TRUE(sorted.has_value()) << (sorted ? "" : sorted.error().to_string());
    EXPECT_EQ(sorted.value().field_type().doc_values_type(), DocValuesType::Sorted);

    auto sorted_set = SortedSetDocValuesField::create("ss", "pear", DocValuesSkipIndexType::None);
    ASSERT_TRUE(sorted_set.has_value()) << (sorted_set ? "" : sorted_set.error().to_string());
    EXPECT_EQ(sorted_set.value().field_type().doc_values_type(), DocValuesType::SortedSet);

    auto numeric = NumericDocValuesField::create("n", 3);
    ASSERT_TRUE(numeric.has_value());
    EXPECT_FALSE(SortedDocValuesField::set_value(numeric.value(), "x").has_value());
    EXPECT_FALSE(SortedSetDocValuesField::set_value(numeric.value(), "x").has_value());
}

TEST(DocValuesFieldsTest, BuildsASortedNumericDocValuesField) {
    auto field = SortedNumericDocValuesField::create("sn", 7);
    ASSERT_TRUE(field.has_value()) << (field ? "" : field.error().to_string());
    EXPECT_EQ(field.value().field_type().doc_values_type(), DocValuesType::SortedNumeric);
    EXPECT_TRUE(SortedNumericDocValuesField::set_value(field.value(), 1).has_value());

    auto reader = index_documents(1, [](Document &doc, int32_t /*doc_id*/) {
        for (const int64_t value : {5, 1, 9}) {
            auto multi = SortedNumericDocValuesField::create("sn", value);
            EXPECT_TRUE(multi.has_value());
            doc.add(std::make_unique<Field>(std::move(multi.value())));
        }
    });
    ASSERT_NE(reader, nullptr);
    auto leaves = reader->leaves();
    ASSERT_TRUE(leaves.has_value());
    auto values = leaves.value()[0].reader()->get_sorted_numeric_doc_values("sn");
    ASSERT_TRUE(values.has_value());
    ASSERT_NE(values.value(), nullptr);
    EXPECT_EQ(values.value()->advance_exact(0).value(), true);
    EXPECT_EQ(values.value()->doc_value_count().value(), 3);
    std::vector<int64_t> read;
    for (int32_t i = 0; i < 3; ++i) {
        read.push_back(values.value()->next_value().value());
    }
    EXPECT_EQ(read, (std::vector<int64_t>{1, 5, 9}));
}

TEST(DocValuesFieldsTest, StoresAnEncodedRangeInBinaryDocValues) {
    auto range = IntRangeDocValuesField::create("r", std::vector<int32_t>{10}, std::vector<int32_t>{20});
    ASSERT_TRUE(range.has_value()) << (range ? "" : range.error().to_string());
    EXPECT_EQ(range.value().field_type().doc_values_type(), DocValuesType::Binary);
    auto bytes = range.value().binary_value();
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(bytes.value().size(), 8U);
    EXPECT_EQ(IntRange::decode_min(bytes.value(), 0).value(), 10);
    EXPECT_EQ(IntRange::decode_max(bytes.value(), 0).value(), 20);

    // The dimension count cannot change.
    auto changed = IntRangeDocValuesField::set_range_values(range.value(), std::vector<int32_t>{1, 2},
                                                            std::vector<int32_t>{3, 4});
    ASSERT_FALSE(changed.has_value());
    EXPECT_EQ(changed.error().kind(), ErrorKind::kIllegalArgument);

    ASSERT_TRUE(IntRangeDocValuesField::set_range_values(range.value(), std::vector<int32_t>{30},
                                                        std::vector<int32_t>{40})
                        .has_value());
    EXPECT_EQ(IntRange::decode_min(range.value().binary_value().value(), 0).value(), 30);
}

TEST(DocValuesFieldsTest, SlowRangeQueryMatchesTheStoredRanges) {
    const std::vector<std::pair<int32_t, int32_t>> ranges{{10, 20}, {15, 25}, {30, 40}};
    auto reader = index_documents(static_cast<int32_t>(ranges.size()), [&ranges](Document &doc, int32_t doc_id) {
        const auto &range = ranges[static_cast<size_t>(doc_id)];
        auto field = IntRangeDocValuesField::create("r", std::vector<int32_t>{range.first},
                                                    std::vector<int32_t>{range.second});
        EXPECT_TRUE(field.has_value());
        doc.add(std::make_unique<Field>(std::move(field.value())));
    });
    ASSERT_NE(reader, nullptr);

    auto query = IntRangeDocValuesField::new_slow_intersects_query("r", std::vector<int32_t>{18},
                                                                   std::vector<int32_t>{22});
    ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());
    auto text = query.value().to_string("r");
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text.value(), "[[18] TO [22]]");
    EXPECT_EQ(search_docs(query.value(), reader), (std::vector<int32_t>{0, 1}));

    auto disjoint = IntRangeDocValuesField::new_slow_intersects_query("r", std::vector<int32_t>{100},
                                                                      std::vector<int32_t>{200});
    ASSERT_TRUE(disjoint.has_value());
    EXPECT_TRUE(search_docs(disjoint.value(), reader).empty());

    // The slow path only supports INTERSECTS.
    auto encode = IntRange::encode(std::vector<int32_t>{1}, std::vector<int32_t>{2});
    ASSERT_TRUE(encode.has_value());
    auto contains = BinaryRangeFieldRangeQuery::create("r", encode.value(), 4, 1, QueryType::Contains, "[[1] TO [2]]");
    ASSERT_FALSE(contains.has_value());
    EXPECT_EQ(contains.error().kind(), ErrorKind::kUnsupportedOperation);
}

TEST(DocValuesFieldsTest, SlowRangeQueryMatchesMultiDimensionalRanges) {
    auto reader = index_documents(2, [](Document &doc, int32_t doc_id) {
        auto field = LongRangeDocValuesField::create(
                "box", doc_id == 0 ? std::vector<int64_t>{0, 0} : std::vector<int64_t>{100, 100},
                doc_id == 0 ? std::vector<int64_t>{10, 10} : std::vector<int64_t>{110, 110});
        EXPECT_TRUE(field.has_value());
        doc.add(std::make_unique<Field>(std::move(field.value())));
    });
    ASSERT_NE(reader, nullptr);

    auto query = LongRangeDocValuesField::new_slow_intersects_query("box", std::vector<int64_t>{5, 5},
                                                                   std::vector<int64_t>{6, 6});
    ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());
    EXPECT_EQ(search_docs(query.value(), reader), (std::vector<int32_t>{0}));

    auto all = LongRangeDocValuesField::new_slow_intersects_query(
            "box", std::vector<int64_t>{std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::min()},
            std::vector<int64_t>{std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::max()});
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(search_docs(all.value(), reader), (std::vector<int32_t>{0, 1}));
}

TEST(DocValuesFieldsTest, SlowRangeQueriesSupportTheOtherNumberTypes) {
    auto dir = std::make_shared<ByteBuffersDirectory>();
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(*dir, IndexWriterConfig(analyzer));
    ASSERT_TRUE(writer.has_value());

    auto float_field = FloatRangeDocValuesField::create("f", std::vector<float>{1.0F}, std::vector<float>{2.0F});
    ASSERT_TRUE(float_field.has_value()) << (float_field ? "" : float_field.error().to_string());
    Document first;
    first.add(std::make_unique<Field>(std::move(float_field.value())));
    ASSERT_TRUE(writer.value()->add_document(first).has_value());

    auto double_field = DoubleRangeDocValuesField::create("d", std::vector<double>{10.0}, std::vector<double>{20.0});
    ASSERT_TRUE(double_field.has_value());
    Document second;
    second.add(std::make_unique<Field>(std::move(double_field.value())));
    ASSERT_TRUE(writer.value()->add_document(second).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto reader = DirectoryReader::open(*dir);
    ASSERT_TRUE(reader.has_value());

    auto float_query = FloatRangeDocValuesField::new_slow_intersects_query("f", std::vector<float>{1.5F},
                                                                          std::vector<float>{1.5F});
    ASSERT_TRUE(float_query.has_value());
    EXPECT_EQ(search_docs(float_query.value(), reader.value()), (std::vector<int32_t>{0}));

    auto double_query = DoubleRangeDocValuesField::new_slow_intersects_query("d", std::vector<double>{15.0},
                                                                            std::vector<double>{15.0});
    ASSERT_TRUE(double_query.has_value());
    EXPECT_EQ(search_docs(double_query.value(), reader.value()), (std::vector<int32_t>{1}));
}

TEST(DocValuesFieldsTest, EncodesFloatAndDoubleBits) {
    auto float_field = FloatDocValuesField::create("f", 1.5F);
    ASSERT_TRUE(float_field.has_value()) << (float_field ? "" : float_field.error().to_string());
    EXPECT_EQ(float_field.value().field_type().doc_values_type(), DocValuesType::Numeric);
    EXPECT_EQ(number_as_i64(float_field.value().numeric_value().value()),
              static_cast<int64_t>(std::bit_cast<int32_t>(1.5F)));

    auto double_field = DoubleDocValuesField::create("d", 2.5);
    ASSERT_TRUE(double_field.has_value());
    EXPECT_EQ(number_as_i64(double_field.value().numeric_value().value()),
              std::bit_cast<int64_t>(2.5));

    auto rejected = FloatDocValuesField::set_value(double_field.value(), 1.0F);
    EXPECT_TRUE(rejected.has_value());
}
