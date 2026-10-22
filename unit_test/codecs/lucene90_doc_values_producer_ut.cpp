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
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.lucene90_doc_values_producer;

import lucene_core;

using namespace lucene;

namespace {

/// The doc-id iteration shared by the writer stubs below.
class DocCursor {
public:
    explicit DocCursor(std::vector<int32_t> docs) : docs_(std::move(docs)) {}

    [[nodiscard]] int32_t doc_id() const { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() {
        if (position_ >= docs_.size()) {
            doc_ = NO_MORE_DOCS;
        } else {
            doc_ = docs_[position_++];
        }
        return doc_;
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) {
        const auto found = std::lower_bound(docs_.begin(), docs_.end(), target);
        position_ = static_cast<size_t>(found - docs_.begin());
        return next_doc();
    }

    [[nodiscard]] Result<bool> advance_exact(int32_t target) {
        const auto found = std::lower_bound(docs_.begin(), docs_.end(), target);
        doc_ = target;
        if (found == docs_.end() || *found != target) {
            position_ = static_cast<size_t>(found - docs_.begin());
            return false;
        }
        position_ = static_cast<size_t>(found - docs_.begin()) + 1;
        return true;
    }

    [[nodiscard]] Result<int64_t> cost() const { return static_cast<int64_t>(docs_.size()); }

protected:
    size_t position() const noexcept { return position_; }

private:
    std::vector<int32_t> docs_;
    size_t position_{0};
    int32_t doc_{-1};
};

class StubNumeric final : public NumericDocValues {
public:
    explicit StubNumeric(std::vector<std::pair<int32_t, int64_t>> values)
        : cursor_(docs_of(values)), values_(std::move(values)) {}

    [[nodiscard]] int32_t doc_id() const override { return cursor_.doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override { return cursor_.next_doc(); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override { return cursor_.advance(target); }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override { return cursor_.advance_exact(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return cursor_.cost(); }

    [[nodiscard]] Result<int64_t> long_value() override {
        const auto doc = cursor_.doc_id();
        // The values are sorted by doc id and the writer asks for them in order, so
        // a binary search keeps the write path linear (a linear scan is quadratic).
        auto found = std::lower_bound(values_.begin(), values_.end(), doc,
                                      [](const auto &entry, int32_t target) { return entry.first < target; });
        if (found == values_.end() || found->first != doc) {
            return err(LuceneError::illegal_state("no value for the current document"));
        }
        return found->second;
    }

private:
    static std::vector<int32_t> docs_of(const std::vector<std::pair<int32_t, int64_t>> &values) {
        std::vector<int32_t> docs;
        docs.reserve(values.size());
        for (const auto &[doc, value] : values) {
            static_cast<void>(value);
            docs.push_back(doc);
        }
        return docs;
    }

    /// The values are sorted by doc id, so a probe is a binary search: a linear
    /// scan makes the writer quadratic.
    [[nodiscard]] auto find_value(int32_t doc) const {
        return std::lower_bound(values_.begin(), values_.end(), doc,
                                [](const auto &entry, int32_t target) { return entry.first < target; });
    }

    DocCursor cursor_;
    std::vector<std::pair<int32_t, int64_t>> values_;
};

class StubBinary final : public BinaryDocValues {
public:
    explicit StubBinary(std::vector<std::pair<int32_t, std::string>> values)
        : cursor_(docs_of(values)), values_(std::move(values)) {}

    [[nodiscard]] int32_t doc_id() const override { return cursor_.doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override { return cursor_.next_doc(); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override { return cursor_.advance(target); }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override { return cursor_.advance_exact(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return cursor_.cost(); }

    [[nodiscard]] Result<BytesRef> binary_value() override {
        const auto doc = cursor_.doc_id();
        auto found = std::lower_bound(values_.begin(), values_.end(), doc,
                                      [](const auto &entry, int32_t target) { return entry.first < target; });
        if (found == values_.end() || found->first != doc) {
            return err(LuceneError::illegal_state("no value for the current document"));
        }
        const std::string &value = found->second;
        return BytesRef(std::vector<uint8_t>(value.begin(), value.end()));
    }

private:
    static std::vector<int32_t> docs_of(const std::vector<std::pair<int32_t, std::string>> &values) {
        std::vector<int32_t> docs;
        docs.reserve(values.size());
        for (const auto &[doc, value] : values) {
            static_cast<void>(value);
            docs.push_back(doc);
        }
        return docs;
    }

    DocCursor cursor_;
    std::vector<std::pair<int32_t, std::string>> values_;
};

class StubSorted final : public SortedDocValues {
public:
    StubSorted(std::vector<std::string> dictionary, std::vector<std::pair<int32_t, int32_t>> ords)
        : cursor_(docs_of(ords)), dictionary_(std::move(dictionary)), ords_(std::move(ords)) {}

    [[nodiscard]] int32_t doc_id() const override { return cursor_.doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override { return cursor_.next_doc(); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override { return cursor_.advance(target); }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override { return cursor_.advance_exact(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return cursor_.cost(); }

    [[nodiscard]] Result<int32_t> ord_value() override {
        const auto doc = cursor_.doc_id();
        auto found = std::lower_bound(ords_.begin(), ords_.end(), doc,
                                      [](const auto &entry, int32_t target) { return entry.first < target; });
        if (found == ords_.end() || found->first != doc) {
            return err(LuceneError::illegal_state("no ord for the current document"));
        }
        return found->second;
    }

    [[nodiscard]] Result<BytesRef> lookup_ord(int32_t ord) override {
        if (ord < 0 || static_cast<size_t>(ord) >= dictionary_.size()) {
            return err(LuceneError::illegal_argument("ordinal out of range"));
        }
        const std::string &term = dictionary_[static_cast<size_t>(ord)];
        return BytesRef(std::vector<uint8_t>(term.begin(), term.end()));
    }

    [[nodiscard]] Result<int32_t> get_value_count() const override {
        return static_cast<int32_t>(dictionary_.size());
    }

private:
    static std::vector<int32_t> docs_of(const std::vector<std::pair<int32_t, int32_t>> &ords) {
        std::vector<int32_t> docs;
        docs.reserve(ords.size());
        for (const auto &[doc, ord] : ords) {
            static_cast<void>(ord);
            docs.push_back(doc);
        }
        return docs;
    }

    DocCursor cursor_;
    std::vector<std::string> dictionary_;
    std::vector<std::pair<int32_t, int32_t>> ords_;
};

class StubSortedNumeric final : public SortedNumericDocValues {
public:
    explicit StubSortedNumeric(std::vector<std::pair<int32_t, std::vector<int64_t>>> values)
        : cursor_(docs_of(values)), values_(std::move(values)) {}

    [[nodiscard]] int32_t doc_id() const override { return cursor_.doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override { return cursor_.next_doc(); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override { return cursor_.advance(target); }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override { return cursor_.advance_exact(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return cursor_.cost(); }

    [[nodiscard]] Result<int64_t> next_value() override {
        const auto &run = current_run();
        if (offset_ >= run.size()) {
            return err(LuceneError::illegal_state("no more values for the current document"));
        }
        return run[offset_++];
    }

    [[nodiscard]] Result<int32_t> doc_value_count() override {
        offset_ = 0;
        return static_cast<int32_t>(current_run().size());
    }

private:
    static std::vector<int32_t> docs_of(const std::vector<std::pair<int32_t, std::vector<int64_t>>> &values) {
        std::vector<int32_t> docs;
        docs.reserve(values.size());
        for (const auto &[doc, run] : values) {
            static_cast<void>(run);
            docs.push_back(doc);
        }
        return docs;
    }

    const std::vector<int64_t> &current_run() const {
        const auto doc = cursor_.doc_id();
        auto found = std::lower_bound(values_.begin(), values_.end(), doc,
                                      [](const auto &entry, int32_t target) { return entry.first < target; });
        if (found == values_.end() || found->first != doc) {
            static const std::vector<int64_t> empty;
            return empty;
        }
        return found->second;
    }

    DocCursor cursor_;
    std::vector<std::pair<int32_t, std::vector<int64_t>>> values_;
    size_t offset_{0};
};

class StubSortedSet final : public SortedSetDocValues {
public:
    StubSortedSet(std::vector<std::string> dictionary,
                  std::vector<std::pair<int32_t, std::vector<int64_t>>> ords)
        : cursor_(docs_of(ords)), dictionary_(std::move(dictionary)), ords_(std::move(ords)) {}

    [[nodiscard]] int32_t doc_id() const override { return cursor_.doc_id(); }
    [[nodiscard]] Result<int32_t> next_doc() override { return cursor_.next_doc(); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override { return cursor_.advance(target); }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override { return cursor_.advance_exact(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return cursor_.cost(); }

    [[nodiscard]] Result<int64_t> next_ord() override {
        const auto &run = current_run();
        if (offset_ >= run.size()) {
            return NO_MORE_ORDS;
        }
        return run[offset_++];
    }

    [[nodiscard]] Result<int32_t> doc_value_count() override {
        offset_ = 0;
        return static_cast<int32_t>(current_run().size());
    }

    [[nodiscard]] Result<BytesRef> lookup_ord(int64_t ord) override {
        if (ord < 0 || static_cast<size_t>(ord) >= dictionary_.size()) {
            return err(LuceneError::illegal_argument("ordinal out of range"));
        }
        const std::string &term = dictionary_[static_cast<size_t>(ord)];
        return BytesRef(std::vector<uint8_t>(term.begin(), term.end()));
    }

    [[nodiscard]] Result<int64_t> get_value_count() const override {
        return static_cast<int64_t>(dictionary_.size());
    }

private:
    static std::vector<int32_t> docs_of(std::vector<std::pair<int32_t, std::vector<int64_t>>> &ords) {
        std::vector<int32_t> docs;
        docs.reserve(ords.size());
        for (const auto &[doc, run] : ords) {
            static_cast<void>(run);
            docs.push_back(doc);
        }
        return docs;
    }

    const std::vector<int64_t> &current_run() const {
        const auto doc = cursor_.doc_id();
        auto found = std::lower_bound(ords_.begin(), ords_.end(), doc,
                                      [](const auto &entry, int32_t target) { return entry.first < target; });
        if (found == ords_.end() || found->first != doc) {
            static const std::vector<int64_t> empty;
            return empty;
        }
        return found->second;
    }

    DocCursor cursor_;
    std::vector<std::string> dictionary_;
    std::vector<std::pair<int32_t, std::vector<int64_t>>> ords_;
    size_t offset_{0};
};

/// Serves a fresh cursor per call, like a real producer.
class StubProducer final : public DocValuesProducer {
public:
    std::optional<std::vector<std::pair<int32_t, int64_t>>> numeric;
    std::optional<std::vector<std::pair<int32_t, std::string>>> binary;
    std::optional<std::pair<std::vector<std::string>, std::vector<std::pair<int32_t, int32_t>>>> sorted;
    std::optional<std::vector<std::pair<int32_t, std::vector<int64_t>>>> sorted_numeric;
    std::optional<std::pair<std::vector<std::string>, std::vector<std::pair<int32_t, std::vector<int64_t>>>>>
            sorted_set;

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric(const FieldInfo &field) const override {
        static_cast<void>(field);
        return std::unique_ptr<NumericDocValues>(new StubNumeric(numeric.value()));
    }

    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> get_binary(const FieldInfo &field) const override {
        static_cast<void>(field);
        return std::unique_ptr<BinaryDocValues>(new StubBinary(binary.value()));
    }

    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted(const FieldInfo &field) const override {
        static_cast<void>(field);
        return std::unique_ptr<SortedDocValues>(
                new StubSorted(sorted.value().first, sorted.value().second));
    }

    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>> get_sorted_numeric(
            const FieldInfo &field) const override {
        static_cast<void>(field);
        return std::unique_ptr<SortedNumericDocValues>(new StubSortedNumeric(sorted_numeric.value()));
    }

    [[nodiscard]] Result<std::unique_ptr<SortedSetDocValues>> get_sorted_set(
            const FieldInfo &field) const override {
        static_cast<void>(field);
        return std::unique_ptr<SortedSetDocValues>(
                new StubSortedSet(sorted_set.value().first, sorted_set.value().second));
    }

    [[nodiscard]] VoidResult check_integrity() const override { return {}; }
    [[nodiscard]] VoidResult close() const override { return {}; }
};

FieldInfo doc_values_field(int32_t number, std::string name, DocValuesType type) {
    FieldInfo field;
    field.number = number;
    field.name = std::move(name);
    field.doc_values_type = type;
    return field;
}

SegmentInfo make_segment(int32_t max_doc) {
    SegmentInfo info;
    info.name = "_0";
    info.id.assign(ID_LENGTH, 0x33);
    info.max_doc = max_doc;
    return info;
}

/// Writes one field with `write` and returns the producer over the result.
template <class Writer>
std::unique_ptr<Lucene90DocValuesProducer> write_and_open(ByteBuffersDirectory &directory, const FieldInfo &field,
                                                         int32_t max_doc, StubProducer &stub, Writer write) {
    auto field_infos = std::make_shared<const FieldInfos>(FieldInfos::of({field}).value());
    SegmentInfo segment_info = make_segment(max_doc);
    SegmentWriteState write_state(directory, field_infos, io_context_default());

    auto consumer = Lucene90DocValuesConsumer::create(write_state, segment_info);
    EXPECT_TRUE(consumer.has_value());
    if (!consumer.has_value()) {
        return nullptr;
    }
    auto written = write(*consumer.value(), field_infos->fields()[0], stub);
    EXPECT_TRUE(written.has_value()) << (written ? "" : written.error().to_string());
    auto closed = consumer.value()->close();
    EXPECT_TRUE(closed.has_value()) << (closed ? "" : closed.error().to_string());

    SegmentReadState read_state(directory, field_infos, io_context_default());
    auto producer = Lucene90DocValuesProducer::create(
            read_state, segment_info, Lucene90DocValuesFormat::DATA_CODEC, Lucene90DocValuesFormat::DATA_EXTENSION,
            Lucene90DocValuesFormat::META_CODEC, Lucene90DocValuesFormat::META_EXTENSION);
    EXPECT_TRUE(producer.has_value()) << (producer ? "" : producer.error().to_string());
    EXPECT_TRUE(producer.value()->check_integrity().has_value());
    return producer.has_value() ? std::move(producer.value()) : nullptr;
}

std::string to_string(const BytesRef &value) {
    const auto bytes = value.as_slice();
    return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

} // namespace

TEST(Lucene90DocValuesProducerTest, ReadsBackADenseNumericField) {
    ByteBuffersDirectory directory;
    const FieldInfo field = doc_values_field(0, "num", DocValuesType::Numeric);
    StubProducer stub;
    stub.numeric = std::vector<std::pair<int32_t, int64_t>>{{0, 11}, {1, 13}, {2, 17}, {3, 19}, {4, 23}};

    auto producer = write_and_open(directory, field, 5, stub,
                                   [](DocValuesConsumer &consumer, const FieldInfo &info, StubProducer &values) {
                                       return consumer.add_numeric_field(info, values);
                                   });
    ASSERT_TRUE(producer != nullptr);

    auto values = producer->get_numeric(field);
    ASSERT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
    const std::vector<int64_t> expected{11, 13, 17, 19, 23};
    for (int32_t doc = 0; doc < 5; ++doc) {
        ASSERT_EQ(values.value()->next_doc().value(), doc);
        auto value = values.value()->long_value();
        ASSERT_TRUE(value.has_value()) << (value ? "" : value.error().to_string());
        EXPECT_EQ(value.value(), expected[static_cast<size_t>(doc)]);
    }
    EXPECT_EQ(values.value()->next_doc().value(), NO_MORE_DOCS);
}

TEST(Lucene90DocValuesProducerTest, ReadsBackASparseNumericField) {
    ByteBuffersDirectory directory;
    const FieldInfo field = doc_values_field(0, "num", DocValuesType::Numeric);
    StubProducer stub;
    stub.numeric = std::vector<std::pair<int32_t, int64_t>>{{0, 5}, {2, 7}, {5, 5}};

    auto producer = write_and_open(directory, field, 8, stub,
                                   [](DocValuesConsumer &consumer, const FieldInfo &info, StubProducer &values) {
                                       return consumer.add_numeric_field(info, values);
                                   });
    ASSERT_TRUE(producer != nullptr);

    auto values = producer->get_numeric(field);
    ASSERT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
    const std::vector<std::pair<int32_t, int64_t>> expected{{0, 5}, {2, 7}, {5, 5}};
    for (const auto &[doc, value] : expected) {
        ASSERT_EQ(values.value()->next_doc().value(), doc);
        auto read = values.value()->long_value();
        ASSERT_TRUE(read.has_value()) << (read ? "" : read.error().to_string());
        EXPECT_EQ(read.value(), value);
    }
    EXPECT_EQ(values.value()->next_doc().value(), NO_MORE_DOCS);
}

TEST(Lucene90DocValuesProducerTest, ReadsBackAConstantNumericField) {
    ByteBuffersDirectory directory;
    const FieldInfo field = doc_values_field(0, "num", DocValuesType::Numeric);
    StubProducer stub;
    stub.numeric = std::vector<std::pair<int32_t, int64_t>>{{0, 9}, {1, 9}, {2, 9}, {3, 9}};

    auto producer = write_and_open(directory, field, 4, stub,
                                   [](DocValuesConsumer &consumer, const FieldInfo &info, StubProducer &values) {
                                       return consumer.add_numeric_field(info, values);
                                   });
    ASSERT_TRUE(producer != nullptr);

    auto values = producer->get_numeric(field);
    ASSERT_TRUE(values.has_value());
    for (int32_t doc = 0; doc < 4; ++doc) {
        ASSERT_EQ(values.value()->next_doc().value(), doc);
        EXPECT_EQ(values.value()->long_value().value(), 9);
    }
}

TEST(Lucene90DocValuesProducerTest, ReadsBackAFixedWidthBinaryField) {
    ByteBuffersDirectory directory;
    const FieldInfo field = doc_values_field(0, "bin", DocValuesType::Binary);
    StubProducer stub;
    stub.binary = std::vector<std::pair<int32_t, std::string>>{{0, "ab"}, {1, "cd"}, {2, "ef"}};

    auto producer = write_and_open(directory, field, 3, stub,
                                   [](DocValuesConsumer &consumer, const FieldInfo &info, StubProducer &values) {
                                       return consumer.add_binary_field(info, values);
                                   });
    ASSERT_TRUE(producer != nullptr);

    auto values = producer->get_binary(field);
    ASSERT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
    const std::vector<std::string> expected{"ab", "cd", "ef"};
    for (int32_t doc = 0; doc < 3; ++doc) {
        ASSERT_EQ(values.value()->next_doc().value(), doc);
        auto value = values.value()->binary_value();
        ASSERT_TRUE(value.has_value()) << (value ? "" : value.error().to_string());
        EXPECT_EQ(to_string(value.value()), expected[static_cast<size_t>(doc)]);
    }
}

TEST(Lucene90DocValuesProducerTest, ReadsBackAVariableWidthBinaryField) {
    ByteBuffersDirectory directory;
    const FieldInfo field = doc_values_field(0, "bin", DocValuesType::Binary);
    StubProducer stub;
    stub.binary = std::vector<std::pair<int32_t, std::string>>{{0, "a"}, {1, "bcd"}, {3, "efghi"}};

    auto producer = write_and_open(directory, field, 5, stub,
                                   [](DocValuesConsumer &consumer, const FieldInfo &info, StubProducer &values) {
                                       return consumer.add_binary_field(info, values);
                                   });
    ASSERT_TRUE(producer != nullptr);

    auto values = producer->get_binary(field);
    ASSERT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
    const std::vector<std::pair<int32_t, std::string>> expected{{0, "a"}, {1, "bcd"}, {3, "efghi"}};
    for (const auto &[doc, text] : expected) {
        ASSERT_EQ(values.value()->next_doc().value(), doc);
        auto value = values.value()->binary_value();
        ASSERT_TRUE(value.has_value()) << (value ? "" : value.error().to_string());
        EXPECT_EQ(to_string(value.value()), text);
    }
}

TEST(Lucene90DocValuesProducerTest, ReadsBackASortedField) {
    ByteBuffersDirectory directory;
    const FieldInfo field = doc_values_field(0, "sorted", DocValuesType::Sorted);
    StubProducer stub;
    stub.sorted = std::make_pair(std::vector<std::string>{"apple", "banana", "cherry"},
                                 std::vector<std::pair<int32_t, int32_t>>{{0, 2}, {1, 0}, {2, 1}});

    auto producer = write_and_open(directory, field, 3, stub,
                                   [](DocValuesConsumer &consumer, const FieldInfo &info, StubProducer &values) {
                                       return consumer.add_sorted_field(info, values);
                                   });
    ASSERT_TRUE(producer != nullptr);

    auto values = producer->get_sorted(field);
    ASSERT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
    EXPECT_EQ(values.value()->get_value_count().value(), 3);

    const std::vector<std::pair<int32_t, std::string>> expected{{0, "cherry"}, {1, "apple"}, {2, "banana"}};
    for (const auto &[doc, term] : expected) {
        ASSERT_EQ(values.value()->next_doc().value(), doc);
        auto ord = values.value()->ord_value();
        ASSERT_TRUE(ord.has_value()) << (ord ? "" : ord.error().to_string());
        auto read = values.value()->lookup_ord(ord.value());
        ASSERT_TRUE(read.has_value()) << (read ? "" : read.error().to_string());
        EXPECT_EQ(to_string(read.value()), term);
    }
}

TEST(Lucene90DocValuesProducerTest, ReadsBackASortedNumericField) {
    ByteBuffersDirectory directory;
    const FieldInfo field = doc_values_field(0, "sorted_num", DocValuesType::SortedNumeric);
    StubProducer stub;
    stub.sorted_numeric =
            std::vector<std::pair<int32_t, std::vector<int64_t>>>{{0, {3, 7}}, {1, {11}}, {2, {13, 17, 19}}};

    auto producer = write_and_open(directory, field, 3, stub,
                                   [](DocValuesConsumer &consumer, const FieldInfo &info, StubProducer &values) {
                                       return consumer.add_sorted_numeric_field(info, values);
                                   });
    ASSERT_TRUE(producer != nullptr);

    auto values = producer->get_sorted_numeric(field);
    ASSERT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
    EXPECT_FALSE(values.value()->is_single_valued());

    const std::vector<std::pair<int32_t, std::vector<int64_t>>> expected{
            {0, {3, 7}}, {1, {11}}, {2, {13, 17, 19}}};
    for (const auto &[doc, run] : expected) {
        ASSERT_EQ(values.value()->next_doc().value(), doc);
        ASSERT_EQ(values.value()->doc_value_count().value(), static_cast<int32_t>(run.size()));
        for (const int64_t value : run) {
            auto read = values.value()->next_value();
            ASSERT_TRUE(read.has_value()) << (read ? "" : read.error().to_string());
            EXPECT_EQ(read.value(), value);
        }
    }
}

TEST(Lucene90DocValuesProducerTest, ReadsBackASingleValuedSortedNumericField) {
    ByteBuffersDirectory directory;
    const FieldInfo field = doc_values_field(0, "sorted_num", DocValuesType::SortedNumeric);
    StubProducer stub;
    stub.sorted_numeric =
            std::vector<std::pair<int32_t, std::vector<int64_t>>>{{0, {2}}, {1, {4}}, {2, {6}}};

    auto producer = write_and_open(directory, field, 3, stub,
                                   [](DocValuesConsumer &consumer, const FieldInfo &info, StubProducer &values) {
                                       return consumer.add_sorted_numeric_field(info, values);
                                   });
    ASSERT_TRUE(producer != nullptr);

    auto values = producer->get_sorted_numeric(field);
    ASSERT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
    EXPECT_TRUE(values.value()->is_single_valued());
    for (int32_t doc = 0; doc < 3; ++doc) {
        ASSERT_EQ(values.value()->next_doc().value(), doc);
        EXPECT_EQ(values.value()->doc_value_count().value(), 1);
        EXPECT_EQ(values.value()->next_value().value(), 2 * (doc + 1));
    }
}

TEST(Lucene90DocValuesProducerTest, ReadsBackASingleValuedSortedSetField) {
    ByteBuffersDirectory directory;
    const FieldInfo field = doc_values_field(0, "sorted_set", DocValuesType::SortedSet);
    StubProducer stub;
    stub.sorted_set = std::make_pair(std::vector<std::string>{"one", "three", "two"},
                                     std::vector<std::pair<int32_t, std::vector<int64_t>>>{
                                             {0, {2}}, {1, {0}}, {2, {1}}});

    auto producer = write_and_open(directory, field, 3, stub,
                                   [](DocValuesConsumer &consumer, const FieldInfo &info, StubProducer &values) {
                                       return consumer.add_sorted_set_field(info, values);
                                   });
    ASSERT_TRUE(producer != nullptr);

    auto values = producer->get_sorted_set(field);
    ASSERT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
    EXPECT_EQ(values.value()->get_value_count().value(), 3);

    const std::vector<std::pair<int32_t, std::string>> expected{{0, "two"}, {1, "one"}, {2, "three"}};
    for (const auto &[doc, term] : expected) {
        ASSERT_EQ(values.value()->next_doc().value(), doc);
        EXPECT_EQ(values.value()->doc_value_count().value(), 1);
        auto ord = values.value()->next_ord();
        ASSERT_TRUE(ord.has_value()) << (ord ? "" : ord.error().to_string());
        auto read = values.value()->lookup_ord(ord.value());
        ASSERT_TRUE(read.has_value()) << (read ? "" : read.error().to_string());
        EXPECT_EQ(to_string(read.value()), term);
        EXPECT_EQ(values.value()->next_ord().value(), NO_MORE_ORDS);
    }
}

TEST(Lucene90DocValuesProducerTest, ReadsBackAMultiValuedSortedSetField) {
    ByteBuffersDirectory directory;
    const FieldInfo field = doc_values_field(0, "sorted_set", DocValuesType::SortedSet);
    StubProducer stub;
    stub.sorted_set = std::make_pair(std::vector<std::string>{"eight", "five", "four", "nine"},
                                     std::vector<std::pair<int32_t, std::vector<int64_t>>>{
                                             {0, {0, 2}}, {1, {1}}, {3, {1, 2, 3}}});

    auto producer = write_and_open(directory, field, 5, stub,
                                   [](DocValuesConsumer &consumer, const FieldInfo &info, StubProducer &values) {
                                       return consumer.add_sorted_set_field(info, values);
                                   });
    ASSERT_TRUE(producer != nullptr);

    auto values = producer->get_sorted_set(field);
    ASSERT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());

    const std::vector<std::pair<int32_t, std::vector<std::string>>> expected{
            {0, {"eight", "four"}}, {1, {"five"}}, {3, {"five", "four", "nine"}}};
    for (const auto &[doc, terms] : expected) {
        ASSERT_EQ(values.value()->next_doc().value(), doc);
        ASSERT_EQ(values.value()->doc_value_count().value(), static_cast<int32_t>(terms.size()));
        for (const std::string &term : terms) {
            auto ord = values.value()->next_ord();
            ASSERT_TRUE(ord.has_value()) << (ord ? "" : ord.error().to_string());
            auto read = values.value()->lookup_ord(ord.value());
            ASSERT_TRUE(read.has_value()) << (read ? "" : read.error().to_string());
            EXPECT_EQ(to_string(read.value()), term);
        }
        EXPECT_EQ(values.value()->next_ord().value(), NO_MORE_ORDS);
    }
    EXPECT_EQ(values.value()->next_doc().value(), NO_MORE_DOCS);
}

TEST(Lucene90DocValuesProducerTest, ReadsBackSeveralFields) {
    ByteBuffersDirectory directory;
    const FieldInfo numeric_field = doc_values_field(0, "num", DocValuesType::Numeric);
    const FieldInfo binary_field = doc_values_field(1, "bin", DocValuesType::Binary);
    auto field_infos = std::make_shared<const FieldInfos>(
            FieldInfos::of({numeric_field, binary_field}).value());
    SegmentInfo segment_info = make_segment(3);
    SegmentWriteState write_state(directory, field_infos, io_context_default());

    StubProducer numeric_stub;
    numeric_stub.numeric = std::vector<std::pair<int32_t, int64_t>>{{0, 1}, {1, 2}, {2, 3}};
    StubProducer binary_stub;
    binary_stub.binary = std::vector<std::pair<int32_t, std::string>>{{0, "x"}, {1, "yz"}, {2, "abc"}};

    auto consumer = Lucene90DocValuesConsumer::create(write_state, segment_info);
    ASSERT_TRUE(consumer.has_value());
    ASSERT_TRUE(consumer.value()->add_numeric_field(field_infos->fields()[0], numeric_stub).has_value());
    ASSERT_TRUE(consumer.value()->add_binary_field(field_infos->fields()[1], binary_stub).has_value());
    ASSERT_TRUE(consumer.value()->close().has_value());

    SegmentReadState read_state(directory, field_infos, io_context_default());
    auto producer = Lucene90DocValuesProducer::create(
            read_state, segment_info, Lucene90DocValuesFormat::DATA_CODEC, Lucene90DocValuesFormat::DATA_EXTENSION,
            Lucene90DocValuesFormat::META_CODEC, Lucene90DocValuesFormat::META_EXTENSION);
    ASSERT_TRUE(producer.has_value()) << (producer ? "" : producer.error().to_string());

    auto numbers = producer.value()->get_numeric(numeric_field);
    ASSERT_TRUE(numbers.has_value());
    for (int32_t doc = 0; doc < 3; ++doc) {
        ASSERT_EQ(numbers.value()->next_doc().value(), doc);
        EXPECT_EQ(numbers.value()->long_value().value(), doc + 1);
    }

    auto texts = producer.value()->get_binary(binary_field);
    ASSERT_TRUE(texts.has_value()) << (texts ? "" : texts.error().to_string());
    const std::vector<std::string> expected{"x", "yz", "abc"};
    for (int32_t doc = 0; doc < 3; ++doc) {
        ASSERT_EQ(texts.value()->next_doc().value(), doc);
        auto value = texts.value()->binary_value();
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(to_string(value.value()), expected[static_cast<size_t>(doc)]);
    }

    // A field without doc values is reported, not silently empty.
    const FieldInfo missing = doc_values_field(7, "missing", DocValuesType::Numeric);
    EXPECT_FALSE(producer.value()->get_numeric(missing).has_value());
}

TEST(Lucene90DocValuesProducerTest, ServesOneCursorPerCall) {
    ByteBuffersDirectory directory;
    const FieldInfo field = doc_values_field(0, "num", DocValuesType::Numeric);
    StubProducer stub;
    stub.numeric = std::vector<std::pair<int32_t, int64_t>>{{0, 1}, {1, 2}};

    auto producer = write_and_open(directory, field, 2, stub,
                                   [](DocValuesConsumer &consumer, const FieldInfo &info, StubProducer &values) {
                                       return consumer.add_numeric_field(info, values);
                                   });
    ASSERT_TRUE(producer != nullptr);

    auto first = producer->get_numeric(field);
    auto second = producer->get_numeric(field);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_EQ(first.value()->next_doc().value(), 0);
    ASSERT_EQ(second.value()->next_doc().value(), 0);
    EXPECT_EQ(first.value()->long_value().value(), 1);
    EXPECT_EQ(second.value()->long_value().value(), 1);

    // A merge instance reads the same values through its own data handle.
    auto merger = producer->get_merge_instance();
    ASSERT_TRUE(merger.has_value()) << (merger ? "" : merger.error().to_string());
    ASSERT_TRUE(merger.value() != nullptr);
    auto merged = merger.value()->get_numeric(field);
    ASSERT_TRUE(merged.has_value());
    ASSERT_EQ(merged.value()->next_doc().value(), 0);
    EXPECT_EQ(merged.value()->long_value().value(), 1);
}

TEST(Lucene90DocValuesProducerTest, ReadsBackBlockCompressedNumerics) {
    // Two blocks of values: each block has a small range, but the blocks are far
    // apart, which is exactly when the writer splits the values into blocks with
    // their own bit width and appends a jump table.
    constexpr int32_t num_docs = 2 * Lucene90DocValuesFormat::NUMERIC_BLOCK_SIZE;
    std::vector<std::pair<int32_t, int64_t>> values;
    values.reserve(num_docs);
    for (int32_t doc = 0; doc < num_docs; ++doc) {
        const int64_t block = doc / Lucene90DocValuesFormat::NUMERIC_BLOCK_SIZE;
        const int64_t within = doc % Lucene90DocValuesFormat::NUMERIC_BLOCK_SIZE;
        values.emplace_back(doc, block * 1000000 + within * 3);
    }

    ByteBuffersDirectory directory;
    const FieldInfo field = doc_values_field(0, "num", DocValuesType::Numeric);
    StubProducer stub;
    stub.numeric = values;

    auto producer = write_and_open(directory, field, num_docs, stub,
                                   [](DocValuesConsumer &consumer, const FieldInfo &info, StubProducer &producer) {
                                       return consumer.add_numeric_field(info, producer);
                                   });
    ASSERT_TRUE(producer != nullptr);

    auto read = producer->get_numeric(field);
    ASSERT_TRUE(read.has_value()) << (read ? "" : read.error().to_string());
    // The values really are split into blocks, i.e. the varying bit-width reader
    // is what the assertions below exercise.
    ASSERT_TRUE(producer->numeric_entry(field.number) != nullptr);
    EXPECT_GE(producer->numeric_entry(field.number)->block_shift, 0);
    for (const auto &[doc, value] : values) {
        ASSERT_EQ(read.value()->next_doc().value(), doc);
        auto got = read.value()->long_value();
        ASSERT_TRUE(got.has_value()) << (got ? "" : got.error().to_string());
        EXPECT_EQ(got.value(), value) << "doc " << doc;
    }
    EXPECT_EQ(read.value()->next_doc().value(), NO_MORE_DOCS);

    // Skipping to the second block uses the jump table instead of scanning.
    auto jumping = producer->get_numeric(field);
    ASSERT_TRUE(jumping.has_value());
    const int32_t target = Lucene90DocValuesFormat::NUMERIC_BLOCK_SIZE + 1;
    ASSERT_EQ(jumping.value()->advance(target).value(), target);
    auto skipped = jumping.value()->long_value();
    ASSERT_TRUE(skipped.has_value());
    EXPECT_EQ(skipped.value(), 1000000 + 3);
}

TEST(Lucene90DocValuesProducerTest, ReadsBackALargeTermDictionary) {
    // More than one block of 64 terms, so the dictionary crosses block boundaries
    // several times.
    constexpr int32_t num_terms = 200;
    std::vector<std::string> dictionary;
    dictionary.reserve(num_terms);
    for (int32_t i = 0; i < num_terms; ++i) {
        dictionary.push_back("term-" + std::to_string(1000 + i));
    }
    std::vector<std::pair<int32_t, int32_t>> ords;
    ords.reserve(num_terms);
    for (int32_t doc = 0; doc < num_terms; ++doc) {
        ords.emplace_back(doc, num_terms - 1 - doc);
    }

    ByteBuffersDirectory directory;
    const FieldInfo field = doc_values_field(0, "sorted", DocValuesType::Sorted);
    StubProducer stub;
    stub.sorted = std::make_pair(dictionary, ords);

    auto producer = write_and_open(directory, field, num_terms, stub,
                                   [](DocValuesConsumer &consumer, const FieldInfo &info, StubProducer &producer) {
                                       return consumer.add_sorted_field(info, producer);
                                   });
    ASSERT_TRUE(producer != nullptr);

    auto read = producer->get_sorted(field);
    ASSERT_TRUE(read.has_value()) << (read ? "" : read.error().to_string());
    EXPECT_EQ(read.value()->get_value_count().value(), num_terms);

    for (const auto &[doc, ord] : ords) {
        ASSERT_EQ(read.value()->next_doc().value(), doc);
        auto got_ord = read.value()->ord_value();
        ASSERT_TRUE(got_ord.has_value());
        EXPECT_EQ(got_ord.value(), ord);
        auto term = read.value()->lookup_ord(ord);
        ASSERT_TRUE(term.has_value()) << (term ? "" : term.error().to_string());
        EXPECT_EQ(to_string(term.value()), dictionary[static_cast<size_t>(ord)]);
    }

    // Random access into the dictionary, including block boundaries.
    for (const int32_t ord : {0, 1, 63, 64, 65, 127, 128, 199}) {
        auto term = read.value()->lookup_ord(ord);
        ASSERT_TRUE(term.has_value()) << (term ? "" : term.error().to_string());
        EXPECT_EQ(to_string(term.value()), dictionary[static_cast<size_t>(ord)]);
    }
}

TEST(Lucene90DocValuesProducerTest, ReadsBackTheSkipIndex) {
    // Two intervals of 4096 documents, so the skipper has to walk past the first.
    constexpr int32_t num_docs = 2 * Lucene90DocValuesFormat::DEFAULT_SKIP_INDEX_INTERVAL_SIZE;
    std::vector<std::pair<int32_t, int64_t>> values;
    values.reserve(num_docs);
    for (int32_t doc = 0; doc < num_docs; ++doc) {
        values.emplace_back(doc, doc);
    }

    ByteBuffersDirectory directory;
    FieldInfo field = doc_values_field(0, "num", DocValuesType::Numeric);
    field.doc_values_skip_index = DocValuesSkipIndexType::Range;
    StubProducer stub;
    stub.numeric = values;

    auto producer = write_and_open(directory, field, num_docs, stub,
                                   [](DocValuesConsumer &consumer, const FieldInfo &info, StubProducer &producer) {
                                       return consumer.add_numeric_field(info, producer);
                                   });
    ASSERT_TRUE(producer != nullptr);

    auto skipper = producer->get_skipper(field);
    ASSERT_TRUE(skipper.has_value()) << (skipper ? "" : skipper.error().to_string());
    ASSERT_TRUE(skipper.value() != nullptr);
    EXPECT_EQ(skipper.value()->global_min_value(), 0);
    EXPECT_EQ(skipper.value()->global_max_value(), num_docs - 1);
    EXPECT_EQ(skipper.value()->global_doc_count(), num_docs);

    ASSERT_TRUE(skipper.value()->advance(0).has_value());
    EXPECT_GE(skipper.value()->num_levels(), 1);
    EXPECT_EQ(skipper.value()->min_doc_id(0), 0);
    EXPECT_GE(skipper.value()->max_doc_id(0), 0);

    // Moving past the first interval rewinds the levels the target no longer fits.
    const int32_t target = Lucene90DocValuesFormat::DEFAULT_SKIP_INDEX_INTERVAL_SIZE;
    ASSERT_TRUE(skipper.value()->advance(target).has_value());
    EXPECT_LE(skipper.value()->min_doc_id(0), target);
    EXPECT_GE(skipper.value()->max_doc_id(0), target);

    // A field without a skip index has no skipper.
    const FieldInfo plain = doc_values_field(1, "plain", DocValuesType::Numeric);
    auto missing = producer->get_skipper(plain);
    ASSERT_TRUE(missing.has_value());
    EXPECT_TRUE(missing.value() == nullptr);
}

TEST(Lucene90DocValuesProducerTest, ExposesTheFormatFactories) {
    Lucene90DocValuesFormat format;
    EXPECT_EQ(format.name(), "Lucene90");

    ByteBuffersDirectory directory;
    const FieldInfo field = doc_values_field(0, "num", DocValuesType::Numeric);
    auto field_infos = std::make_shared<const FieldInfos>(FieldInfos::of({field}).value());
    SegmentInfo segment_info = make_segment(2);
    SegmentWriteState write_state(directory, field_infos, io_context_default());

    StubProducer stub;
    stub.numeric = std::vector<std::pair<int32_t, int64_t>>{{0, 42}, {1, 43}};

    auto consumer = format.fields_consumer(write_state, segment_info);
    ASSERT_TRUE(consumer.has_value()) << (consumer ? "" : consumer.error().to_string());
    ASSERT_TRUE(consumer.value()->add_numeric_field(field_infos->fields()[0], stub).has_value());
    ASSERT_TRUE(consumer.value()->close().has_value());

    SegmentReadState read_state(directory, field_infos, io_context_default());
    auto producer = format.fields_producer(read_state, segment_info);
    ASSERT_TRUE(producer.has_value()) << (producer ? "" : producer.error().to_string());
    auto values = producer.value()->get_numeric(field);
    ASSERT_TRUE(values.has_value());
    ASSERT_EQ(values.value()->next_doc().value(), 0);
    EXPECT_EQ(values.value()->long_value().value(), 42);
    ASSERT_EQ(values.value()->next_doc().value(), 1);
    EXPECT_EQ(values.value()->long_value().value(), 43);
}
