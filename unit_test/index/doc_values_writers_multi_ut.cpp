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
#include <memory>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.doc_values_writers_multi;

import lucene_core;

using namespace lucene;

namespace {

using BinaryEntries = std::vector<std::pair<int32_t, std::string>>;
using NumericEntries = std::vector<std::pair<int32_t, std::vector<int64_t>>>;

/// Collects the binary and sorted-numeric fields handed to a consumer.
class RecordingConsumer final : public DocValuesConsumer {
public:
    [[nodiscard]] VoidResult add_binary_field(const FieldInfo &field, DocValuesProducer &producer) override {
        auto values = producer.get_binary(field);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        BinaryEntries entries;
        while (true) {
            auto doc = values.value()->next_doc();
            if (!doc.has_value()) {
                return std::unexpected(std::move(doc.error()));
            }
            if (doc.value() == NO_MORE_DOCS) {
                break;
            }
            auto value = values.value()->binary_value();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            const auto bytes = value.value().as_slice();
            entries.emplace_back(doc.value(), std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
        }
        binary.push_back(std::move(entries));
        binary_fields.push_back(field.name);
        return {};
    }

    [[nodiscard]] VoidResult add_sorted_numeric_field(const FieldInfo &field,
                                                      DocValuesProducer &producer) override {
        auto values = producer.get_sorted_numeric(field);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        NumericEntries entries;
        while (true) {
            auto doc = values.value()->next_doc();
            if (!doc.has_value()) {
                return std::unexpected(std::move(doc.error()));
            }
            if (doc.value() == NO_MORE_DOCS) {
                break;
            }
            auto count = values.value()->doc_value_count();
            if (!count.has_value()) {
                return std::unexpected(std::move(count.error()));
            }
            std::vector<int64_t> per_doc;
            for (int32_t i = 0; i < count.value(); ++i) {
                auto value = values.value()->next_value();
                if (!value.has_value()) {
                    return std::unexpected(std::move(value.error()));
                }
                per_doc.push_back(value.value());
            }
            entries.emplace_back(doc.value(), std::move(per_doc));
        }
        single_valued.push_back(values.value()->is_single_valued());
        sorted_numeric.push_back(std::move(entries));
        return {};
    }

    [[nodiscard]] VoidResult add_numeric_field(const FieldInfo &, DocValuesProducer &) override {
        return err(LuceneError::unsupported_operation("numeric doc values are not expected"));
    }
    [[nodiscard]] VoidResult add_sorted_field(const FieldInfo &, DocValuesProducer &) override {
        return err(LuceneError::unsupported_operation("sorted doc values are not expected"));
    }
    [[nodiscard]] VoidResult add_sorted_set_field(const FieldInfo &, DocValuesProducer &) override {
        return err(LuceneError::unsupported_operation("sorted set doc values are not expected"));
    }

    std::vector<std::string> binary_fields;
    std::vector<BinaryEntries> binary;
    std::vector<NumericEntries> sorted_numeric;
    std::vector<bool> single_valued;
};

std::shared_ptr<FieldInfo> field_with(std::string name, DocValuesType type) {
    auto field = std::make_shared<FieldInfo>();
    field->name = std::move(name);
    field->doc_values_type = type;
    return field;
}

std::span<const uint8_t> as_bytes(std::string_view text) {
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

} // namespace

TEST(BinaryDocValuesWriterTest, WritesBufferedValues) {
    auto field = field_with("blob", DocValuesType::Binary);
    BinaryDocValuesWriter writer(field);
    ASSERT_TRUE(writer.add_value(0, as_bytes("aa")).has_value());
    ASSERT_TRUE(writer.add_value(2, as_bytes("bbbb")).has_value());
    ASSERT_TRUE(writer.add_value(3, as_bytes("")).has_value());
    EXPECT_EQ(writer.max_length(), 4U);
    EXPECT_EQ(writer.num_docs_with_field(), 3);

    RecordingConsumer consumer;
    ASSERT_TRUE(writer.flush(consumer).has_value());
    ASSERT_EQ(consumer.binary_fields, (std::vector<std::string>{"blob"}));
    EXPECT_EQ(consumer.binary[0], (BinaryEntries{{0, "aa"}, {2, "bbbb"}, {3, ""}}));
}

TEST(BinaryDocValuesWriterTest, RejectsRepeatedDocs) {
    BinaryDocValuesWriter writer(field_with("blob", DocValuesType::Binary));
    ASSERT_TRUE(writer.add_value(1, as_bytes("a")).has_value());
    auto repeated = writer.add_value(1, as_bytes("b"));
    ASSERT_FALSE(repeated.has_value());
    EXPECT_EQ(repeated.error().kind(), ErrorKind::kIllegalArgument);
    EXPECT_FALSE(writer.add_value(0, as_bytes("c")).has_value());
}

TEST(SortedNumericDocValuesWriterTest, SortsValuesWithinADocument) {
    auto field = field_with("sizes", DocValuesType::SortedNumeric);
    SortedNumericDocValuesWriter writer(field);
    // Deliberately unsorted input.
    ASSERT_TRUE(writer.add_value(0, 5).has_value());
    ASSERT_TRUE(writer.add_value(0, 3).has_value());
    ASSERT_TRUE(writer.add_value(0, 9).has_value());
    ASSERT_TRUE(writer.add_value(1, 7).has_value());
    ASSERT_TRUE(writer.add_value(3, 1).has_value());
    ASSERT_TRUE(writer.add_value(3, 2).has_value());

    RecordingConsumer consumer;
    ASSERT_TRUE(writer.flush(consumer).has_value());
    ASSERT_EQ(consumer.sorted_numeric.size(), 1U);
    EXPECT_EQ(consumer.sorted_numeric[0], (NumericEntries{{0, {3, 5, 9}}, {1, {7}}, {3, {1, 2}}}));
    // Multiple values exist, so the single-valued shortcut must not be used.
    EXPECT_FALSE(consumer.single_valued[0]);
}

TEST(SortedNumericDocValuesWriterTest, SingleValuedDocsUseTheCompactShape) {
    auto field = field_with("sizes", DocValuesType::SortedNumeric);
    SortedNumericDocValuesWriter writer(field);
    ASSERT_TRUE(writer.add_value(0, 4).has_value());
    ASSERT_TRUE(writer.add_value(1, 6).has_value());
    ASSERT_TRUE(writer.add_value(2, 8).has_value());

    RecordingConsumer consumer;
    ASSERT_TRUE(writer.flush(consumer).has_value());
    EXPECT_TRUE(consumer.single_valued[0]);
    EXPECT_EQ(consumer.sorted_numeric[0], (NumericEntries{{0, {4}}, {1, {6}}, {2, {8}}}));
}

TEST(SortedNumericDocValuesWriterTest, CountsCoverDocumentsSeenBeforeTheFirstMultiValuedOne) {
    auto field = field_with("sizes", DocValuesType::SortedNumeric);
    SortedNumericDocValuesWriter writer(field);
    ASSERT_TRUE(writer.add_value(0, 1).has_value());
    ASSERT_TRUE(writer.add_value(1, 2).has_value());
    // The count stream is created lazily here; docs 0 and 1 must default to 1.
    ASSERT_TRUE(writer.add_value(2, 3).has_value());
    ASSERT_TRUE(writer.add_value(2, 4).has_value());

    RecordingConsumer consumer;
    ASSERT_TRUE(writer.flush(consumer).has_value());
    EXPECT_EQ(consumer.sorted_numeric[0], (NumericEntries{{0, {1}}, {1, {2}}, {2, {3, 4}}}));
    EXPECT_FALSE(consumer.single_valued[0]);
}

TEST(SortedNumericDocValuesWriterTest, EmptyWriterProducesEmptyField) {
    SortedNumericDocValuesWriter writer(field_with("sizes", DocValuesType::SortedNumeric));
    RecordingConsumer consumer;
    ASSERT_TRUE(writer.flush(consumer).has_value());
    ASSERT_EQ(consumer.sorted_numeric.size(), 1U);
    EXPECT_TRUE(consumer.sorted_numeric[0].empty());
    EXPECT_TRUE(consumer.single_valued[0]);
}
