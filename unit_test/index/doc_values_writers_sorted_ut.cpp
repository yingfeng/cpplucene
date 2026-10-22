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

module lucene_core:ut.doc_values_writers_sorted;

import lucene_core;

using namespace lucene;

namespace {

/// One decoded sorted(-set) field: the dictionary plus the per-doc ordinals.
struct DecodedSorted {
    std::vector<std::string> dictionary;
    std::vector<std::pair<int32_t, std::vector<int64_t>>> docs;
    int64_t value_count{0};
};

class RecordingConsumer final : public DocValuesConsumer {
public:
    [[nodiscard]] VoidResult add_sorted_field(const FieldInfo &field, DocValuesProducer &producer) override {
        auto values = producer.get_sorted(field);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        DecodedSorted decoded;
        auto count = values.value()->get_value_count();
        if (!count.has_value()) {
            return std::unexpected(std::move(count.error()));
        }
        decoded.value_count = count.value();
        for (int32_t ord = 0; ord < count.value(); ++ord) {
            auto term = values.value()->lookup_ord(ord);
            if (!term.has_value()) {
                return std::unexpected(std::move(term.error()));
            }
            decoded.dictionary.push_back(to_string(term.value()));
        }

        while (true) {
            auto doc = values.value()->next_doc();
            if (!doc.has_value()) {
                return std::unexpected(std::move(doc.error()));
            }
            if (doc.value() == NO_MORE_DOCS) {
                break;
            }
            auto ord = values.value()->ord_value();
            if (!ord.has_value()) {
                return std::unexpected(std::move(ord.error()));
            }
            decoded.docs.emplace_back(doc.value(), std::vector<int64_t>{ord.value()});
        }
        sorted.push_back(std::move(decoded));
        return {};
    }

    [[nodiscard]] VoidResult add_sorted_set_field(const FieldInfo &field, DocValuesProducer &producer) override {
        auto values = producer.get_sorted_set(field);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        DecodedSorted decoded;
        auto count = values.value()->get_value_count();
        if (!count.has_value()) {
            return std::unexpected(std::move(count.error()));
        }
        decoded.value_count = count.value();
        for (int64_t ord = 0; ord < count.value(); ++ord) {
            auto term = values.value()->lookup_ord(ord);
            if (!term.has_value()) {
                return std::unexpected(std::move(term.error()));
            }
            decoded.dictionary.push_back(to_string(term.value()));
        }

        while (true) {
            auto doc = values.value()->next_doc();
            if (!doc.has_value()) {
                return std::unexpected(std::move(doc.error()));
            }
            if (doc.value() == NO_MORE_DOCS) {
                break;
            }
            auto doc_count = values.value()->doc_value_count();
            if (!doc_count.has_value()) {
                return std::unexpected(std::move(doc_count.error()));
            }
            std::vector<int64_t> ords;
            for (int32_t i = 0; i < doc_count.value(); ++i) {
                auto ord = values.value()->next_ord();
                if (!ord.has_value()) {
                    return std::unexpected(std::move(ord.error()));
                }
                ords.push_back(ord.value());
            }
            auto end = values.value()->next_ord();
            if (!end.has_value() || end.value() != NO_MORE_ORDS) {
                return err(LuceneError::illegal_state("next_ord must report NO_MORE_ORDS after the last ord"));
            }
            decoded.docs.emplace_back(doc.value(), std::move(ords));
        }
        sorted_set.push_back(std::move(decoded));
        return {};
    }

    [[nodiscard]] VoidResult add_numeric_field(const FieldInfo &, DocValuesProducer &) override {
        return err(LuceneError::unsupported_operation("numeric doc values are not expected"));
    }
    [[nodiscard]] VoidResult add_binary_field(const FieldInfo &, DocValuesProducer &) override {
        return err(LuceneError::unsupported_operation("binary doc values are not expected"));
    }
    [[nodiscard]] VoidResult add_sorted_numeric_field(const FieldInfo &, DocValuesProducer &) override {
        return err(LuceneError::unsupported_operation("sorted numeric doc values are not expected"));
    }

    static std::string to_string(const BytesRef &value) {
        const auto bytes = value.as_slice();
        return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    }

    std::vector<DecodedSorted> sorted;
    std::vector<DecodedSorted> sorted_set;
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

TEST(SortedDocValuesWriterTest, BuildsASortedDictionaryAndOrdinals) {
    SortedDocValuesWriter writer(field_with("tag", DocValuesType::Sorted));
    ASSERT_TRUE(writer.add_value(0, as_bytes("banana")).has_value());
    ASSERT_TRUE(writer.add_value(1, as_bytes("apple")).has_value());
    ASSERT_TRUE(writer.add_value(2, as_bytes("banana")).has_value());
    EXPECT_EQ(writer.num_docs_with_field(), 3);

    RecordingConsumer consumer;
    ASSERT_TRUE(writer.flush(consumer).has_value());
    ASSERT_EQ(consumer.sorted.size(), 1U);
    const auto &decoded = consumer.sorted[0];
    EXPECT_EQ(decoded.dictionary, (std::vector<std::string>{"apple", "banana"}));
    EXPECT_EQ(decoded.value_count, 2);
    EXPECT_EQ(decoded.docs, (std::vector<std::pair<int32_t, std::vector<int64_t>>>{
                                    {0, {1}}, {1, {0}}, {2, {1}}}));
}

TEST(SortedDocValuesWriterTest, RejectsRepeatedDocs) {
    SortedDocValuesWriter writer(field_with("tag", DocValuesType::Sorted));
    ASSERT_TRUE(writer.add_value(0, as_bytes("a")).has_value());
    auto repeated = writer.add_value(0, as_bytes("b"));
    ASSERT_FALSE(repeated.has_value());
    EXPECT_EQ(repeated.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(SortedDocValuesWriterTest, EmptyWriterProducesAnEmptyDictionary) {
    SortedDocValuesWriter writer(field_with("tag", DocValuesType::Sorted));
    RecordingConsumer consumer;
    ASSERT_TRUE(writer.flush(consumer).has_value());
    ASSERT_EQ(consumer.sorted.size(), 1U);
    EXPECT_TRUE(consumer.sorted[0].dictionary.empty());
    EXPECT_EQ(consumer.sorted[0].value_count, 0);
    EXPECT_TRUE(consumer.sorted[0].docs.empty());
}

TEST(SortedSetDocValuesWriterTest, DeduplicatesAndSortsOrdinalsPerDocument) {
    SortedSetDocValuesWriter writer(field_with("tags", DocValuesType::SortedSet));
    // Doc 0 has the values out of order and with a duplicate.
    ASSERT_TRUE(writer.add_value(0, as_bytes("b")).has_value());
    ASSERT_TRUE(writer.add_value(0, as_bytes("a")).has_value());
    ASSERT_TRUE(writer.add_value(0, as_bytes("b")).has_value());
    ASSERT_TRUE(writer.add_value(1, as_bytes("c")).has_value());
    ASSERT_TRUE(writer.add_value(3, as_bytes("a")).has_value());
    EXPECT_EQ(writer.num_docs_with_field(), 3);

    RecordingConsumer consumer;
    ASSERT_TRUE(writer.flush(consumer).has_value());
    ASSERT_EQ(consumer.sorted_set.size(), 1U);
    const auto &decoded = consumer.sorted_set[0];
    EXPECT_EQ(decoded.dictionary, (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_EQ(decoded.value_count, 3);
    EXPECT_EQ(decoded.docs, (std::vector<std::pair<int32_t, std::vector<int64_t>>>{
                                    {0, {0, 1}}, {1, {2}}, {3, {0}}}));
}

TEST(SortedSetDocValuesWriterTest, SingleValuedDocsStillReportCounts) {
    SortedSetDocValuesWriter writer(field_with("tags", DocValuesType::SortedSet));
    ASSERT_TRUE(writer.add_value(0, as_bytes("x")).has_value());
    ASSERT_TRUE(writer.add_value(1, as_bytes("y")).has_value());

    RecordingConsumer consumer;
    ASSERT_TRUE(writer.flush(consumer).has_value());
    EXPECT_EQ(consumer.sorted_set[0].dictionary, (std::vector<std::string>{"x", "y"}));
    EXPECT_EQ(consumer.sorted_set[0].docs, (std::vector<std::pair<int32_t, std::vector<int64_t>>>{
                                                   {0, {0}}, {1, {1}}}));
}

TEST(SortedSetDocValuesWriterTest, EmptyWriterProducesAnEmptyDictionary) {
    SortedSetDocValuesWriter writer(field_with("tags", DocValuesType::SortedSet));
    RecordingConsumer consumer;
    ASSERT_TRUE(writer.flush(consumer).has_value());
    ASSERT_EQ(consumer.sorted_set.size(), 1U);
    EXPECT_TRUE(consumer.sorted_set[0].dictionary.empty());
    EXPECT_TRUE(consumer.sorted_set[0].docs.empty());
}
