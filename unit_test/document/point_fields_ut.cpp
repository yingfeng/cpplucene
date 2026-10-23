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

module lucene_core:ut.point_fields;

import lucene_core;

using namespace lucene;

namespace {

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

template <typename Add>
std::shared_ptr<DirectoryReader> index_documents(int32_t count, Add add) {
    auto dir = std::make_shared<ByteBuffersDirectory>();
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(*dir, IndexWriterConfig(analyzer));
    EXPECT_TRUE(writer.has_value());
    if (!writer.has_value()) {
        return nullptr;
    }
    for (int32_t doc_id = 0; doc_id < count; ++doc_id) {
        Document doc;
        auto field = add(doc_id);
        EXPECT_TRUE(field.has_value()) << (field ? "" : field.error().to_string());
        doc.add(std::make_unique<Field>(std::move(field.value())));
        auto added = writer.value()->add_document(doc);
        EXPECT_TRUE(added.has_value()) << (added ? "" : added.error().to_string());
    }
    EXPECT_TRUE(writer.value()->commit().has_value());
    auto reader = DirectoryReader::open(*dir);
    EXPECT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    return reader.has_value() ? reader.value() : nullptr;
}

} // namespace

TEST(PointFieldsTest, PacksIntPointsWithTheSortableEncoding) {
    auto packed = IntPoint::pack(std::vector<int32_t>{1});
    ASSERT_TRUE(packed.has_value()) << (packed ? "" : packed.error().to_string());
    EXPECT_EQ(packed.value(), (std::vector<uint8_t>{0x80, 0x00, 0x00, 0x01}));

    auto negative = IntPoint::pack(std::vector<int32_t>{-1});
    ASSERT_TRUE(negative.has_value());
    EXPECT_EQ(negative.value(), (std::vector<uint8_t>{0x7F, 0xFF, 0xFF, 0xFF}));

    auto field = IntPoint::create("p", std::vector<int32_t>{10, 20});
    ASSERT_TRUE(field.has_value());
    EXPECT_EQ(field.value().field_type().point_dimension_count(), 2U);
    EXPECT_EQ(field.value().field_type().point_num_bytes(), 4U);
    EXPECT_EQ(field.value().binary_value().value().size(), 8U);
}

TEST(PointFieldsTest, RejectsInvalidPoints) {
    auto empty = IntPoint::create("p", std::vector<int32_t>{});
    ASSERT_FALSE(empty.has_value());
    EXPECT_EQ(empty.error().kind(), ErrorKind::kIllegalArgument);

    auto too_many = LongPoint::create("p", std::vector<int64_t>{1, 2, 3, 4, 5, 6, 7, 8, 9});
    ASSERT_FALSE(too_many.has_value());

    auto mixed = BinaryPoint::create("p", std::vector<std::vector<uint8_t>>{{1, 2}, {3}});
    ASSERT_FALSE(mixed.has_value());

    auto too_wide = BinaryPoint::create("p", std::vector<std::vector<uint8_t>>{{1, 2, 3, 4, 5, 6, 7, 8, 9}});
    ASSERT_FALSE(too_wide.has_value());

    auto empty_set = IntPoint::new_set_query("p", std::vector<int32_t>{});
    ASSERT_FALSE(empty_set.has_value());
}

TEST(PointFieldsTest, QueriesIntPoints) {
    auto reader = index_documents(4, [](int32_t doc_id) {
        return IntPoint::create("p", std::vector<int32_t>{doc_id * 10});
    });
    ASSERT_NE(reader, nullptr);

    auto exact = IntPoint::new_exact_query("p", 20);
    ASSERT_TRUE(exact.has_value()) << (exact ? "" : exact.error().to_string());
    EXPECT_EQ(search_docs(*exact.value(), reader), (std::vector<int32_t>{2}));

    auto set = IntPoint::new_set_query("p", std::vector<int32_t>{0, 30});
    ASSERT_TRUE(set.has_value());
    EXPECT_EQ(search_docs(*set.value(), reader), (std::vector<int32_t>{0, 3}));

    auto range = IntPoint::new_range_query("p", 10, 20);
    ASSERT_TRUE(range.has_value());
    EXPECT_EQ(search_docs(*range.value(), reader), (std::vector<int32_t>{1, 2}));
}

TEST(PointFieldsTest, QueriesMultiDimensionalIntPoints) {
    auto reader = index_documents(3, [](int32_t doc_id) {
        const std::vector<int32_t> point{doc_id * 10, doc_id * 10 + 1};
        return IntPoint::create("p", point);
    });
    ASSERT_NE(reader, nullptr);

    auto query = IntPoint::new_range_query_n("p", std::vector<int32_t>{5, 6}, std::vector<int32_t>{15, 16});
    ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());
    EXPECT_EQ(search_docs(*query.value(), reader), (std::vector<int32_t>{1}));

    auto all = IntPoint::new_range_query_n("p", std::vector<int32_t>{-100, -100},
                                          std::vector<int32_t>{100, 100});
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(search_docs(*all.value(), reader), (std::vector<int32_t>{0, 1, 2}));

    auto mismatched = IntPoint::new_range_query_n("p", std::vector<int32_t>{1, 2}, std::vector<int32_t>{3});
    ASSERT_FALSE(mismatched.has_value());
}

TEST(PointFieldsTest, QueriesLongFloatAndDoublePoints) {
    auto long_reader = index_documents(3, [](int32_t doc_id) {
        return LongPoint::create("l", std::vector<int64_t>{static_cast<int64_t>(doc_id) * 1'000'000'000});
    });
    ASSERT_NE(long_reader, nullptr);
    auto long_query = LongPoint::new_range_query("l", 0, 1'000'000'000);
    ASSERT_TRUE(long_query.has_value()) << (long_query ? "" : long_query.error().to_string());
    EXPECT_EQ(search_docs(*long_query.value(), long_reader), (std::vector<int32_t>{0, 1}));

    auto float_reader = index_documents(3, [](int32_t doc_id) {
        return FloatPoint::create("f", std::vector<float>{static_cast<float>(doc_id) * 0.5F});
    });
    ASSERT_NE(float_reader, nullptr);
    auto float_exact = FloatPoint::new_exact_query("f", 0.5F);
    ASSERT_TRUE(float_exact.has_value());
    EXPECT_EQ(search_docs(*float_exact.value(), float_reader), (std::vector<int32_t>{1}));

    auto double_reader = index_documents(3, [](int32_t doc_id) {
        return DoublePoint::create("d", std::vector<double>{static_cast<double>(doc_id) * 2.5});
    });
    ASSERT_NE(double_reader, nullptr);
    auto double_range = DoublePoint::new_range_query("d", 0.0, 5.0);
    ASSERT_TRUE(double_range.has_value());
    EXPECT_EQ(search_docs(*double_range.value(), double_reader), (std::vector<int32_t>{0, 1, 2}));
    auto double_set = DoublePoint::new_set_query("d", std::vector<double>{0.0, 5.0});
    ASSERT_TRUE(double_set.has_value());
    EXPECT_EQ(search_docs(*double_set.value(), double_reader), (std::vector<int32_t>{0, 2}));
}

TEST(PointFieldsTest, QueriesBinaryPoints) {
    // One dimension of two bytes: {0,0}, {1,0}, {2,0}.
    auto reader = index_documents(3, [](int32_t doc_id) {
        const std::vector<std::vector<uint8_t>> point{{static_cast<uint8_t>(doc_id), 0}};
        return BinaryPoint::create("b", point);
    });
    ASSERT_NE(reader, nullptr);

    auto exact = BinaryPoint::new_exact_query("b", std::vector<uint8_t>{2, 0});
    ASSERT_TRUE(exact.has_value()) << (exact ? "" : exact.error().to_string());
    EXPECT_EQ(search_docs(*exact.value(), reader), (std::vector<int32_t>{2}));

    auto range = BinaryPoint::new_range_query("b", std::vector<uint8_t>{1, 0}, std::vector<uint8_t>{2, 0});
    ASSERT_TRUE(range.has_value());
    EXPECT_EQ(search_docs(*range.value(), reader), (std::vector<int32_t>{1, 2}));

    auto set = BinaryPoint::new_set_query("b", std::vector<std::vector<uint8_t>>{{0, 0}, {2, 0}});
    ASSERT_TRUE(set.has_value());
    EXPECT_EQ(search_docs(*set.value(), reader), (std::vector<int32_t>{0, 2}));

    // Two dimensions of one byte each.
    auto multi = index_documents(3, [](int32_t doc_id) {
        const std::vector<std::vector<uint8_t>> point{{static_cast<uint8_t>(doc_id)},
                                                      {static_cast<uint8_t>(doc_id * 2)}};
        return BinaryPoint::create("b", point);
    });
    ASSERT_NE(multi, nullptr);
    auto multi_query = BinaryPoint::new_range_query_n("b", std::vector<std::vector<uint8_t>>{{1}, {2}},
                                                      std::vector<std::vector<uint8_t>>{{1}, {2}});
    ASSERT_TRUE(multi_query.has_value()) << (multi_query ? "" : multi_query.error().to_string());
    EXPECT_EQ(search_docs(*multi_query.value(), multi), (std::vector<int32_t>{1}));
}
