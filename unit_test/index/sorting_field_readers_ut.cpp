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

module lucene_core:ut.sorting_field_readers;

import lucene_core;

using namespace lucene;

namespace {

/// A stored fields reader over `values`; the document of id `i` holds one field
/// named `doc` with that value.
class FakeStoredFieldsReader final : public StoredFieldsReader {
public:
    explicit FakeStoredFieldsReader(std::vector<std::string> values) : values_(std::move(values)) {}

    /// The document ids this reader was asked for, in order.
    std::vector<int32_t> requested;

    [[nodiscard]] VoidResult prefetch(int32_t doc_id) override {
        requested.push_back(doc_id);
        return {};
    }

    [[nodiscard]] VoidResult document_with_visitor(int32_t doc_id, StoredFieldVisitor &/*visitor*/) override {
        requested.push_back(doc_id);
        return {};
    }

    [[nodiscard]] Result<Document> document(int32_t doc_id) override {
        requested.push_back(doc_id);
        if (doc_id < 0 || static_cast<size_t>(doc_id) >= values_.size()) {
            return err(LuceneError::illegal_argument("doc id out of range"));
        }
        Document doc;
        doc.add(std::make_unique<Field>(StoredField::from_string("doc", values_[static_cast<size_t>(doc_id)])));
        return doc;
    }

    [[nodiscard]] VoidResult check_integrity() const override { return {}; }

    [[nodiscard]] Result<std::unique_ptr<StoredFieldsReader>> get_merge_instance() const override {
        return std::unique_ptr<StoredFieldsReader>();
    }

    [[nodiscard]] Result<std::unique_ptr<StoredFieldsReader>> clone() const override {
        return std::unique_ptr<StoredFieldsReader>(new FakeStoredFieldsReader(values_));
    }

    [[nodiscard]] VoidResult close() const override { return {}; }

private:
    std::vector<std::string> values_;
};

/// A term vectors reader that records which documents it was asked about.
class FakeTermVectorsReader final : public TermVectorsReader {
public:
    std::vector<int32_t> requested;

    [[nodiscard]] VoidResult prefetch(int32_t doc_id) override {
        requested.push_back(doc_id);
        return {};
    }

    [[nodiscard]] Result<std::unique_ptr<Fields>> get(int32_t doc) override {
        requested.push_back(doc);
        return std::unique_ptr<Fields>();
    }

    [[nodiscard]] VoidResult check_integrity() const override { return {}; }

    [[nodiscard]] Result<std::unique_ptr<TermVectorsReader>> get_merge_instance() const override {
        return std::unique_ptr<TermVectorsReader>();
    }

    [[nodiscard]] Result<std::unique_ptr<TermVectorsReader>> clone() const override {
        return std::unique_ptr<TermVectorsReader>(new FakeTermVectorsReader());
    }

    [[nodiscard]] VoidResult close() const override { return {}; }
};

/// The map that reverses the documents of a three-document segment.
[[nodiscard]] std::shared_ptr<const IndexSortDocMap> reversed_map() {
    return std::make_shared<const IndexSortDocMapImpl>(std::vector<int32_t>{2, 1, 0});
}

} // namespace

TEST(SortingStoredFieldsReaderTest, ReadsDocumentsInTheSortedOrder) {
    auto fake = std::make_shared<FakeStoredFieldsReader>(std::vector<std::string>{"first", "second", "third"});
    auto reader = SortingStoredFieldsReader::create(fake, reversed_map());
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());

    // Sorted document 0 is old document 2, and so on.
    auto doc = reader.value()->document(0);
    ASSERT_TRUE(doc.has_value()) << (doc ? "" : doc.error().to_string());
    EXPECT_EQ(doc.value().get("doc").value().value(), "third");

    auto second = reader.value()->document(1);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second.value().get("doc").value().value(), "second");

    auto last = reader.value()->document(2);
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last.value().get("doc").value().value(), "first");

    // Prefetch goes through the same mapping.
    ASSERT_TRUE(reader.value()->prefetch(0).has_value());
    EXPECT_EQ(fake->requested.back(), 2);
}

TEST(SortingStoredFieldsReaderTest, ClonesShareTheMapping) {
    auto fake = std::make_shared<FakeStoredFieldsReader>(std::vector<std::string>{"first", "second"});
    auto reader = SortingStoredFieldsReader::create(fake, std::make_shared<const IndexSortDocMapImpl>(
                                                               std::vector<int32_t>{1, 0}));
    ASSERT_TRUE(reader.has_value());

    // The fake reports no merge instance, so the wrapper clones instead.
    auto instance = reader.value()->get_merge_instance();
    ASSERT_TRUE(instance.has_value()) << (instance ? "" : instance.error().to_string());
    ASSERT_TRUE(instance.value() != nullptr);
    auto doc = instance.value()->document(0);
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc.value().get("doc").value().value(), "second");

    auto copy = reader.value()->clone();
    ASSERT_TRUE(copy.has_value());
    auto copied = copy.value()->document(1);
    ASSERT_TRUE(copied.has_value());
    EXPECT_EQ(copied.value().get("doc").value().value(), "first");
}

TEST(SortingStoredFieldsReaderTest, RejectsOutOfRangeDocuments) {
    auto fake = std::make_shared<FakeStoredFieldsReader>(std::vector<std::string>{"only"});
    auto reader = SortingStoredFieldsReader::create(fake, std::make_shared<const IndexSortDocMapImpl>(
                                                               std::vector<int32_t>{0}));
    ASSERT_TRUE(reader.has_value());
    EXPECT_FALSE(reader.value()->document(1).has_value());
    EXPECT_FALSE(reader.value()->prefetch(-1).has_value());
}

TEST(SortingTermVectorsReaderTest, ReadsVectorsInTheSortedOrder) {
    auto fake = std::make_shared<FakeTermVectorsReader>();
    auto reader = SortingTermVectorsReader::create(fake, reversed_map());
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());

    ASSERT_TRUE(reader.value()->get(0).has_value());
    ASSERT_TRUE(reader.value()->get(2).has_value());
    ASSERT_TRUE(reader.value()->prefetch(1).has_value());
    EXPECT_EQ(fake->requested, (std::vector<int32_t>{2, 0, 1}));

    EXPECT_FALSE(reader.value()->get(3).has_value());
}

TEST(SortingFieldReadersTest, RejectsNullArguments) {
    auto fake = std::make_shared<FakeStoredFieldsReader>(std::vector<std::string>{"only"});
    EXPECT_FALSE(SortingStoredFieldsReader::create(nullptr, reversed_map()).has_value());
    EXPECT_FALSE(SortingStoredFieldsReader::create(fake, nullptr).has_value());
    EXPECT_FALSE(SortingTermVectorsReader::create(nullptr, reversed_map()).has_value());
    EXPECT_FALSE(SortingTermVectorsReader::create(std::make_shared<FakeTermVectorsReader>(), nullptr).has_value());
}
