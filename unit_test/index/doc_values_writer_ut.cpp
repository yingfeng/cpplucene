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
#include <set>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.doc_values_writer;

import lucene_core;

using namespace lucene;

namespace {

/// Collects what the writers hand to a [`DocValuesConsumer`].
class RecordingDocValuesConsumer final : public DocValuesConsumer {
public:
    using Entries = std::vector<std::pair<int32_t, int64_t>>;

    [[nodiscard]] VoidResult add_numeric_field(const FieldInfo &field, DocValuesProducer &producer) override {
        auto values = producer.get_numeric(field);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        Entries entries;
        while (true) {
            auto doc = values.value()->next_doc();
            if (!doc.has_value()) {
                return std::unexpected(std::move(doc.error()));
            }
            if (doc.value() == NO_MORE_DOCS) {
                break;
            }
            auto value = values.value()->long_value();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            entries.emplace_back(doc.value(), value.value());
        }
        names.push_back(field.name);
        numeric_values.push_back(std::move(entries));
        return {};
    }

    [[nodiscard]] VoidResult add_binary_field(const FieldInfo &, DocValuesProducer &) override {
        return err(LuceneError::unsupported_operation("binary doc values are not expected"));
    }
    [[nodiscard]] VoidResult add_sorted_field(const FieldInfo &, DocValuesProducer &) override {
        return err(LuceneError::unsupported_operation("sorted doc values are not expected"));
    }
    [[nodiscard]] VoidResult add_sorted_numeric_field(const FieldInfo &, DocValuesProducer &) override {
        return err(LuceneError::unsupported_operation("sorted numeric doc values are not expected"));
    }
    [[nodiscard]] VoidResult add_sorted_set_field(const FieldInfo &, DocValuesProducer &) override {
        return err(LuceneError::unsupported_operation("sorted set doc values are not expected"));
    }

    std::vector<std::string> names;
    std::vector<Entries> numeric_values;
};

/// Collects the norms a [`NormsConsumer`] receives.
class RecordingNormsConsumer final : public NormsConsumer {
public:
    using Entries = std::vector<std::pair<int32_t, int64_t>>;

    [[nodiscard]] VoidResult add_norms_field(const FieldInfo &field, NormsProducer &producer) override {
        auto values = producer.get_norms(field);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        Entries entries;
        while (true) {
            auto doc = values.value()->next_doc();
            if (!doc.has_value()) {
                return std::unexpected(std::move(doc.error()));
            }
            if (doc.value() == NO_MORE_DOCS) {
                break;
            }
            auto value = values.value()->long_value();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            entries.emplace_back(doc.value(), value.value());
        }
        names.push_back(field.name);
        norms.push_back(std::move(entries));
        return {};
    }

    std::vector<std::string> names;
    std::vector<Entries> norms;
};

std::shared_ptr<FieldInfo> numeric_field(std::string name) {
    auto field = std::make_shared<FieldInfo>();
    field->name = std::move(name);
    field->doc_values_type = DocValuesType::Numeric;
    return field;
}

} // namespace

TEST(NumericDocValuesWriterTest, WritesBufferedValuesForDocsWithValues) {
    auto field = numeric_field("price");
    NumericDocValuesWriter writer(field);
    ASSERT_TRUE(writer.add_value(0, 7).has_value());
    ASSERT_TRUE(writer.add_value(2, 42).has_value());
    ASSERT_TRUE(writer.add_value(5, 3).has_value());
    EXPECT_EQ(writer.num_docs_with_field(), 3);

    ASSERT_TRUE(writer.finish().has_value());
    RecordingDocValuesConsumer consumer;
    ASSERT_TRUE(writer.flush(consumer).has_value());

    ASSERT_EQ(consumer.names, (std::vector<std::string>{"price"}));
    ASSERT_EQ(consumer.numeric_values.size(), 1U);
    // The docs-with-field set skips docs 1, 3 and 4; the values stay in step.
    EXPECT_EQ(consumer.numeric_values[0], (RecordingDocValuesConsumer::Entries{{0, 7}, {2, 42}, {5, 3}}));
}

TEST(NumericDocValuesWriterTest, FlushImpliesFinish) {
    auto field = numeric_field("count");
    NumericDocValuesWriter writer(field);
    ASSERT_TRUE(writer.add_value(0, 11).has_value());
    ASSERT_TRUE(writer.add_value(1, 12).has_value());

    RecordingDocValuesConsumer consumer;
    ASSERT_TRUE(writer.flush(consumer).has_value());
    EXPECT_EQ(consumer.numeric_values[0], (RecordingDocValuesConsumer::Entries{{0, 11}, {1, 12}}));
}

TEST(NumericDocValuesWriterTest, RejectsDuplicateAndDecreasingDocs) {
    NumericDocValuesWriter writer(numeric_field("price"));
    ASSERT_TRUE(writer.add_value(2, 1).has_value());
    auto duplicate = writer.add_value(2, 2);
    ASSERT_FALSE(duplicate.has_value());
    EXPECT_EQ(duplicate.error().kind(), ErrorKind::kIllegalArgument);
    EXPECT_FALSE(writer.add_value(1, 3).has_value());
}

TEST(NumericDocValuesWriterTest, EmptyWriterProducesEmptyField) {
    NumericDocValuesWriter writer(numeric_field("empty"));
    ASSERT_TRUE(writer.finish().has_value());
    RecordingDocValuesConsumer consumer;
    ASSERT_TRUE(writer.flush(consumer).has_value());
    EXPECT_EQ(consumer.names, (std::vector<std::string>{"empty"}));
    EXPECT_TRUE(consumer.numeric_values[0].empty());
}

TEST(NormValuesWriterTest, WritesNormsThroughTheNormsConsumer) {
    auto field = std::make_shared<FieldInfo>();
    field->name = "body";
    field->index_options = IndexOptions::DocsAndFreqs;

    NormValuesWriter writer(field);
    ASSERT_TRUE(writer.add_value(0, 4).has_value());
    ASSERT_TRUE(writer.add_value(1, 9).has_value());
    ASSERT_TRUE(writer.add_value(3, 1).has_value());

    ASSERT_TRUE(writer.finish().has_value());
    RecordingNormsConsumer consumer;
    ASSERT_TRUE(writer.flush_norms(consumer).has_value());
    ASSERT_EQ(consumer.names, (std::vector<std::string>{"body"}));
    EXPECT_EQ(consumer.norms[0], (RecordingNormsConsumer::Entries{{0, 4}, {1, 9}, {3, 1}}));
}

TEST(NormValuesWriterTest, RefusesToBeWrittenAsDocValues) {
    NormValuesWriter writer(numeric_field("norm"));
    ASSERT_TRUE(writer.add_value(0, 1).has_value());
    RecordingDocValuesConsumer consumer;
    auto flushed = writer.flush(consumer);
    ASSERT_FALSE(flushed.has_value());
    EXPECT_EQ(flushed.error().kind(), ErrorKind::kUnsupportedOperation);
}

TEST(NormValuesWriterTest, RejectsRepeatedDocs) {
    NormValuesWriter writer(numeric_field("norm"));
    ASSERT_TRUE(writer.add_value(0, 1).has_value());
    EXPECT_FALSE(writer.add_value(0, 2).has_value());
}

TEST(FieldInfosBuilderTest, AssignsNumbersInInsertionOrder) {
    FieldInfosBuilder builder;
    FieldInfo title;
    title.name = "title";
    title.index_options = IndexOptions::DocsAndFreqs;
    FieldInfo body;
    body.name = "body";
    body.index_options = IndexOptions::DocsAndFreqsAndPositions;

    auto first = builder.add(title);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first.value()->number, 0);
    auto second = builder.add(body);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second.value()->number, 1);
    EXPECT_EQ(builder.size(), 2);

    ASSERT_NE(builder.field_info("title"), nullptr);
    EXPECT_EQ(builder.field_info("title")->number, 0);
    EXPECT_EQ(builder.field_info("missing"), nullptr);
    EXPECT_EQ(builder.get_field_names(), (std::set<std::string>{"body", "title"}));

    auto infos = builder.finish();
    ASSERT_TRUE(infos.has_value());
    ASSERT_EQ(infos.value().size(), 2);
    ASSERT_NE(infos.value().by_name("body"), nullptr);
    EXPECT_EQ(infos.value().by_number(1)->name, "body");
}

TEST(FieldInfosBuilderTest, MergesSameNamedFields) {
    FieldInfosBuilder builder;
    FieldInfo first;
    first.name = "body";
    first.index_options = IndexOptions::Docs;
    auto added = builder.add(first);
    ASSERT_TRUE(added.has_value());
    ASSERT_TRUE(added.value()->omit_norms == false);

    FieldInfo second;
    second.name = "body";
    second.index_options = IndexOptions::DocsAndFreqsAndPositionsAndOffsets;
    second.omit_norms = true;
    second.store_term_vector = true;
    second.doc_values_type = DocValuesType::Sorted;
    auto merged = builder.add(second);
    ASSERT_TRUE(merged.has_value());
    EXPECT_EQ(merged.value()->number, 0);
    EXPECT_EQ(merged.value()->index_options, IndexOptions::DocsAndFreqsAndPositionsAndOffsets);
    EXPECT_TRUE(merged.value()->omit_norms);
    EXPECT_TRUE(merged.value()->store_term_vector);
    EXPECT_EQ(merged.value()->doc_values_type, DocValuesType::Sorted);
    // The builder mutates in place, so the earlier handle sees the merge too.
    EXPECT_EQ(added.value()->index_options, IndexOptions::DocsAndFreqsAndPositionsAndOffsets);
    EXPECT_EQ(builder.size(), 1);
}

TEST(FieldInfosBuilderTest, RejectsConflictingDocValuesAndExplicitNumbers) {
    FieldInfosBuilder builder;
    FieldInfo numeric;
    numeric.name = "field";
    numeric.doc_values_type = DocValuesType::Numeric;
    ASSERT_TRUE(builder.add(numeric).has_value());

    FieldInfo sorted = numeric;
    sorted.doc_values_type = DocValuesType::Sorted;
    auto conflict = builder.add(sorted);
    ASSERT_FALSE(conflict.has_value());
    EXPECT_EQ(conflict.error().kind(), ErrorKind::kIllegalArgument);

    FieldInfo numbered;
    numbered.name = "other";
    numbered.number = 5;
    auto rejected = builder.add(numbered);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(FieldInfosBuilderTest, TracksSoftDeletesAndClear) {
    FieldInfosBuilder builder;
    FieldInfo docs;
    docs.name = "docs";
    ASSERT_TRUE(builder.add(docs).has_value());
    FieldInfo soft;
    soft.name = "_soft_deletes";
    soft.soft_deletes = true;
    ASSERT_TRUE(builder.add(soft).has_value());

    ASSERT_TRUE(builder.soft_deletes_field().has_value());
    EXPECT_EQ(builder.soft_deletes_field().value(), "_soft_deletes");
    EXPECT_TRUE(builder.is_soft_deletes_field_name("_soft_deletes"));
    EXPECT_FALSE(builder.is_soft_deletes_field_name("docs"));
    EXPECT_FALSE(builder.parent_field().has_value());

    builder.clear();
    EXPECT_EQ(builder.size(), 0);
    auto empty = builder.finish();
    ASSERT_TRUE(empty.has_value());
    EXPECT_EQ(empty.value().size(), 0);
}
