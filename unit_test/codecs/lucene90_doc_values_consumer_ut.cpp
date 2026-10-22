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

module lucene_core:ut.lucene90_doc_values_consumer;

import lucene_core;

using namespace lucene;

namespace {

/// Doc ids with one value each, the shape a numeric field takes.
class PairsNumeric final : public NumericDocValues {
public:
    using Pairs = std::vector<std::pair<int32_t, int64_t>>;

    explicit PairsNumeric(Pairs pairs) : pairs_(std::move(pairs)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override {
        if (position_ >= pairs_.size()) {
            doc_ = NO_MORE_DOCS;
        } else {
            doc_ = pairs_[position_++].first;
        }
        return doc_;
    }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        auto found = std::lower_bound(pairs_.begin(), pairs_.end(), target,
                                      [](const auto &pair, int32_t value) { return pair.first < value; });
        position_ = static_cast<size_t>(found - pairs_.begin());
        return next_doc();
    }
    [[nodiscard]] Result<int32_t> slow_advance(int32_t target) override { return advance(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(pairs_.size()); }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        auto found = std::lower_bound(pairs_.begin(), pairs_.end(), target,
                                      [](const auto &pair, int32_t value) { return pair.first < value; });
        doc_ = target;
        if (found == pairs_.end() || found->first != target) {
            position_ = static_cast<size_t>(found - pairs_.begin());
            return false;
        }
        position_ = static_cast<size_t>(found - pairs_.begin()) + 1;
        return true;
    }
    [[nodiscard]] Result<int64_t> long_value() override {
        if (position_ == 0) {
            return err(LuceneError::illegal_state("no document is positioned"));
        }
        return pairs_[position_ - 1].second;
    }
    [[nodiscard]] const Pairs &pairs() const noexcept { return pairs_; }

private:
    Pairs pairs_;
    size_t position_{0};
    int32_t doc_{-1};
};

/// One binary value per doc id.
class PairsBinary final : public BinaryDocValues {
public:
    using Pairs = std::vector<std::pair<int32_t, std::string>>;

    explicit PairsBinary(Pairs pairs) : pairs_(std::move(pairs)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override {
        if (position_ >= pairs_.size()) {
            doc_ = NO_MORE_DOCS;
        } else {
            doc_ = pairs_[position_++].first;
        }
        return doc_;
    }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        auto found = std::lower_bound(pairs_.begin(), pairs_.end(), target,
                                      [](const auto &pair, int32_t value) { return pair.first < value; });
        position_ = static_cast<size_t>(found - pairs_.begin());
        return next_doc();
    }
    [[nodiscard]] Result<int32_t> slow_advance(int32_t target) override { return advance(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(pairs_.size()); }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        auto found = std::lower_bound(pairs_.begin(), pairs_.end(), target,
                                      [](const auto &pair, int32_t value) { return pair.first < value; });
        doc_ = target;
        if (found == pairs_.end() || found->first != target) {
            position_ = static_cast<size_t>(found - pairs_.begin());
            return false;
        }
        position_ = static_cast<size_t>(found - pairs_.begin()) + 1;
        return true;
    }
    [[nodiscard]] Result<BytesRef> binary_value() override {
        if (position_ == 0) {
            return err(LuceneError::illegal_state("no document is positioned"));
        }
        const std::string &value = pairs_[position_ - 1].second;
        return BytesRef(std::vector<uint8_t>(value.begin(), value.end()));
    }

private:
    Pairs pairs_;
    size_t position_{0};
    int32_t doc_{-1};
};

/// A dictionary plus one ordinal per doc id.
class DictionarySorted final : public SortedDocValues {
public:
    DictionarySorted(std::vector<std::string> dictionary, std::vector<std::pair<int32_t, int32_t>> pairs)
        : dictionary_(std::move(dictionary)), pairs_(std::move(pairs)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override {
        if (position_ >= pairs_.size()) {
            doc_ = NO_MORE_DOCS;
        } else {
            doc_ = pairs_[position_++].first;
        }
        return doc_;
    }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        auto found = std::lower_bound(pairs_.begin(), pairs_.end(), target,
                                      [](const auto &pair, int32_t value) { return pair.first < value; });
        position_ = static_cast<size_t>(found - pairs_.begin());
        return next_doc();
    }
    [[nodiscard]] Result<int32_t> slow_advance(int32_t target) override { return advance(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(pairs_.size()); }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        auto found = std::lower_bound(pairs_.begin(), pairs_.end(), target,
                                      [](const auto &pair, int32_t value) { return pair.first < value; });
        doc_ = target;
        if (found == pairs_.end() || found->first != target) {
            position_ = static_cast<size_t>(found - pairs_.begin());
            return false;
        }
        position_ = static_cast<size_t>(found - pairs_.begin()) + 1;
        return true;
    }
    [[nodiscard]] Result<int32_t> ord_value() override {
        if (position_ == 0) {
            return err(LuceneError::illegal_state("no document is positioned"));
        }
        return pairs_[position_ - 1].second;
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
    std::vector<std::string> dictionary_;
    std::vector<std::pair<int32_t, int32_t>> pairs_;
    size_t position_{0};
    int32_t doc_{-1};
};

/// Serves one prepared values instance per call, i.e. a producer that always
/// starts from the beginning.
class TestProducer final : public DocValuesProducer {
public:
    void set_numeric(std::shared_ptr<PairsNumeric::Pairs> pairs) { numeric_ = std::move(pairs); }
    void set_binary(std::shared_ptr<PairsBinary::Pairs> pairs) { binary_ = std::move(pairs); }
    void set_sorted(std::vector<std::string> dictionary, std::vector<std::pair<int32_t, int32_t>> pairs) {
        dictionary_ = std::move(dictionary);
        sorted_pairs_ = std::move(pairs);
    }

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric(const FieldInfo &field) const override {
        static_cast<void>(field);
        return std::unique_ptr<NumericDocValues>(new PairsNumeric(*numeric_));
    }
    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> get_binary(const FieldInfo &field) const override {
        static_cast<void>(field);
        return std::unique_ptr<BinaryDocValues>(new PairsBinary(*binary_));
    }
    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted(const FieldInfo &field) const override {
        static_cast<void>(field);
        return std::unique_ptr<SortedDocValues>(new DictionarySorted(dictionary_, sorted_pairs_));
    }
    [[nodiscard]] VoidResult check_integrity() const override { return {}; }
    [[nodiscard]] VoidResult close() const override { return {}; }

private:
    std::shared_ptr<PairsNumeric::Pairs> numeric_;
    std::shared_ptr<PairsBinary::Pairs> binary_;
    std::vector<std::string> dictionary_;
    std::vector<std::pair<int32_t, int32_t>> sorted_pairs_;
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
    info.id.assign(ID_LENGTH, 0x77);
    info.max_doc = max_doc;
    return info;
}

/// Reads the `.dvm` metadata of a single-field segment through a small cursor.
class MetaReader {
public:
    explicit MetaReader(std::unique_ptr<IndexInput> input) : input_(std::move(input)) {}

    int32_t read_int() { return expect(input_->read_int()); }
    int16_t read_short() { return expect(input_->read_short()); }
    int64_t read_long() { return expect(input_->read_long()); }
    uint8_t read_byte() { return expect(input_->read_byte()); }
    int64_t read_vlong() { return expect(input_->read_vlong()); }

    IndexInput &input() { return *input_; }

private:
    template <class T>
    T expect(Result<T> value) {
        EXPECT_TRUE(value.has_value()) << (value ? "" : value.error().to_string());
        return value.has_value() ? value.value() : T{};
    }

    std::unique_ptr<IndexInput> input_;
};

std::unique_ptr<MetaReader> open_meta(Directory &directory, const SegmentInfo &segment_info) {
    auto input = directory.open_input("_0.dvm", io_context_default());
    EXPECT_TRUE(input.has_value());
    // Skip the header so the cursor sits on the first field number.
    auto header = CodecUtil::check_index_header(*input.value(), Lucene90DocValuesFormat::META_CODEC,
                                                Lucene90DocValuesFormat::VERSION_START,
                                                Lucene90DocValuesFormat::VERSION_CURRENT,
                                                std::span<const uint8_t>(segment_info.id), "");
    EXPECT_TRUE(header.has_value()) << (header ? "" : header.error().to_string());
    return std::make_unique<MetaReader>(std::move(input.value()));
}

/// The metadata a `DirectMonotonicWriter` leaves behind for one block.
struct MonotonicMeta {
    int64_t min;
    int32_t average_increment_bits;
    int64_t data_offset;
    uint8_t bits_per_value;
};

MonotonicMeta read_monotonic_meta(MetaReader &meta) {
    MonotonicMeta result{};
    result.min = meta.read_long();
    result.average_increment_bits = meta.read_int();
    result.data_offset = meta.read_long();
    result.bits_per_value = meta.read_byte();
    return result;
}

} // namespace

TEST(Lucene90DocValuesConsumerTest, WritesASparseNumericField) {
    ByteBuffersDirectory directory;
    auto field_infos = std::make_shared<const FieldInfos>(
            FieldInfos::of({doc_values_field(0, "num", DocValuesType::Numeric)}).value());
    SegmentInfo segment_info = make_segment(8);
    SegmentWriteState state(directory, field_infos, io_context_default());

    auto source = std::make_shared<PairsNumeric::Pairs>();
    *source = {{0, 5}, {2, 7}, {5, 5}};
    TestProducer producer;
    producer.set_numeric(source);

    auto consumer = Lucene90DocValuesConsumer::create(state, segment_info);
    ASSERT_TRUE(consumer.has_value()) << (consumer ? "" : consumer.error().to_string());
    ASSERT_TRUE(consumer.value()->add_numeric_field(field_infos->fields()[0], producer).has_value());
    ASSERT_TRUE(consumer.value()->close().has_value());

    auto names = directory.list_all();
    ASSERT_TRUE(names.has_value());
    EXPECT_NE(std::find(names->begin(), names->end(), "_0.dvd"), names->end());
    EXPECT_NE(std::find(names->begin(), names->end(), "_0.dvm"), names->end());

    auto meta = open_meta(directory, segment_info);
    EXPECT_EQ(meta->read_int(), 0);                                  // field number
    EXPECT_EQ(meta->read_byte(), Lucene90DocValuesFormat::NUMERIC);   // type
    const int64_t docs_with_field_offset = meta->read_long();
    EXPECT_GT(docs_with_field_offset, 0);  // a sparse field writes an IndexedDISI
    EXPECT_GT(meta->read_long(), 0);       // docsWithFieldLength
    EXPECT_GE(meta->read_short(), 0);      // jumpTableEntryCount
    EXPECT_EQ(meta->read_byte(), static_cast<uint8_t>(DEFAULT_DENSE_RANK_POWER));
    EXPECT_EQ(meta->read_long(), 3);   // numValues
    EXPECT_EQ(meta->read_int(), -1);   // neither a unique-values table nor blocks
    EXPECT_EQ(meta->read_byte(), 1);   // (7 - 5) / gcd needs a single bit
    EXPECT_EQ(meta->read_long(), 5);   // min
    EXPECT_EQ(meta->read_long(), 2);   // gcd of (7 - 5)
    EXPECT_GT(meta->read_long(), 0);       // values offset
    EXPECT_GT(meta->read_long(), 0);       // values length
    EXPECT_EQ(meta->read_long(), -1);      // no block jump table
    EXPECT_EQ(meta->read_int(), -1);       // end-of-fields marker
}

TEST(Lucene90DocValuesConsumerTest, WritesAConstantNumericField) {
    ByteBuffersDirectory directory;
    auto field_infos = std::make_shared<const FieldInfos>(
            FieldInfos::of({doc_values_field(0, "num", DocValuesType::Numeric)}).value());
    SegmentInfo segment_info = make_segment(4);
    SegmentWriteState state(directory, field_infos, io_context_default());

    auto source = std::make_shared<PairsNumeric::Pairs>();
    *source = {{0, 9}, {1, 9}, {2, 9}, {3, 9}};
    TestProducer producer;
    producer.set_numeric(source);

    auto consumer = Lucene90DocValuesConsumer::create(state, segment_info);
    ASSERT_TRUE(consumer.has_value());
    ASSERT_TRUE(consumer.value()->add_numeric_field(field_infos->fields()[0], producer).has_value());
    ASSERT_TRUE(consumer.value()->close().has_value());

    auto meta = open_meta(directory, segment_info);
    EXPECT_EQ(meta->read_int(), 0);
    EXPECT_EQ(meta->read_byte(), Lucene90DocValuesFormat::NUMERIC);
    EXPECT_EQ(meta->read_long(), -1);  // every document has a value
    EXPECT_EQ(meta->read_long(), 0);
    EXPECT_EQ(meta->read_short(), -1);
    EXPECT_EQ(meta->read_byte(), static_cast<uint8_t>(-1));
    EXPECT_EQ(meta->read_long(), 4);   // numValues
    EXPECT_EQ(meta->read_int(), -1);   // neither a unique-values table nor blocks
    EXPECT_EQ(meta->read_byte(), 0);   // no bits per value: it is constant
    EXPECT_EQ(meta->read_long(), 9);   // the constant itself
    EXPECT_EQ(meta->read_long(), 0);   // a constant has no GCD
    EXPECT_GT(meta->read_long(), 0);   // values offset, even though nothing is stored
    EXPECT_EQ(meta->read_long(), 0);   // values length: the value lives in the metadata
    EXPECT_EQ(meta->read_long(), -1);  // no block jump table
    EXPECT_EQ(meta->read_int(), -1);   // end-of-fields marker
}

TEST(Lucene90DocValuesConsumerTest, WritesAFixedWidthBinaryField) {
    ByteBuffersDirectory directory;
    auto field_infos = std::make_shared<const FieldInfos>(
            FieldInfos::of({doc_values_field(0, "bin", DocValuesType::Binary)}).value());
    SegmentInfo segment_info = make_segment(3);
    SegmentWriteState state(directory, field_infos, io_context_default());

    auto source = std::make_shared<PairsBinary::Pairs>();
    *source = {{0, "ab"}, {1, "cd"}, {2, "ef"}};
    TestProducer producer;
    producer.set_binary(source);

    auto consumer = Lucene90DocValuesConsumer::create(state, segment_info);
    ASSERT_TRUE(consumer.has_value());
    ASSERT_TRUE(consumer.value()->add_binary_field(field_infos->fields()[0], producer).has_value());
    ASSERT_TRUE(consumer.value()->close().has_value());

    auto meta = open_meta(directory, segment_info);
    EXPECT_EQ(meta->read_int(), 0);
    EXPECT_EQ(meta->read_byte(), Lucene90DocValuesFormat::BINARY);
    EXPECT_GT(meta->read_long(), 0);  // data offset
    EXPECT_EQ(meta->read_long(), 6);  // data length
    EXPECT_EQ(meta->read_long(), -1);  // all documents have a value
    EXPECT_EQ(meta->read_long(), 0);
    EXPECT_EQ(meta->read_short(), -1);
    EXPECT_EQ(meta->read_byte(), static_cast<uint8_t>(-1));
    EXPECT_EQ(meta->read_int(), 3);  // numDocsWithField
    EXPECT_EQ(meta->read_int(), 2);  // minLength
    EXPECT_EQ(meta->read_int(), 2);  // maxLength: fixed width, so no addresses
}

TEST(Lucene90DocValuesConsumerTest, WritesAVariableWidthBinaryField) {
    ByteBuffersDirectory directory;
    auto field_infos = std::make_shared<const FieldInfos>(
            FieldInfos::of({doc_values_field(0, "bin", DocValuesType::Binary)}).value());
    SegmentInfo segment_info = make_segment(2);
    SegmentWriteState state(directory, field_infos, io_context_default());

    auto source = std::make_shared<PairsBinary::Pairs>();
    *source = {{0, "a"}, {1, "bcd"}};
    TestProducer producer;
    producer.set_binary(source);

    auto consumer = Lucene90DocValuesConsumer::create(state, segment_info);
    ASSERT_TRUE(consumer.has_value());
    ASSERT_TRUE(consumer.value()->add_binary_field(field_infos->fields()[0], producer).has_value());
    ASSERT_TRUE(consumer.value()->close().has_value());

    auto meta = open_meta(directory, segment_info);
    EXPECT_EQ(meta->read_int(), 0);
    EXPECT_EQ(meta->read_byte(), Lucene90DocValuesFormat::BINARY);
    EXPECT_GT(meta->read_long(), 0);
    EXPECT_EQ(meta->read_long(), 4);
    EXPECT_EQ(meta->read_long(), -1);
    EXPECT_EQ(meta->read_long(), 0);
    EXPECT_EQ(meta->read_short(), -1);
    EXPECT_EQ(meta->read_byte(), static_cast<uint8_t>(-1));
    EXPECT_EQ(meta->read_int(), 2);
    EXPECT_EQ(meta->read_int(), 1);  // minLength
    EXPECT_EQ(meta->read_int(), 3);  // maxLength differs, so addresses follow
    EXPECT_GT(meta->read_long(), 0);  // addresses offset
    EXPECT_EQ(meta->read_vlong(), Lucene90DocValuesFormat::DIRECT_MONOTONIC_BLOCK_SHIFT);
    EXPECT_EQ(read_monotonic_meta(*meta).min, -1);  // the residuals are 0, -1 and 0
    EXPECT_GT(meta->read_long(), 0);  // addresses length
    EXPECT_EQ(meta->read_int(), -1);  // end-of-fields marker
}

TEST(Lucene90DocValuesConsumerTest, WritesASortedField) {
    ByteBuffersDirectory directory;
    auto field_infos = std::make_shared<const FieldInfos>(
            FieldInfos::of({doc_values_field(0, "sorted", DocValuesType::Sorted)}).value());
    SegmentInfo segment_info = make_segment(3);
    SegmentWriteState state(directory, field_infos, io_context_default());

    TestProducer producer;
    producer.set_sorted({"apple", "banana", "cherry"}, {{0, 2}, {1, 0}, {2, 1}});

    auto consumer = Lucene90DocValuesConsumer::create(state, segment_info);
    ASSERT_TRUE(consumer.has_value());
    auto added = consumer.value()->add_sorted_field(field_infos->fields()[0], producer);
    ASSERT_TRUE(added.has_value()) << (added ? "" : added.error().to_string());
    ASSERT_TRUE(consumer.value()->close().has_value());

    auto meta = open_meta(directory, segment_info);
    EXPECT_EQ(meta->read_int(), 0);
    EXPECT_EQ(meta->read_byte(), Lucene90DocValuesFormat::SORTED);
    // Ordinals: every document has one, so no doc-id set is written.
    EXPECT_EQ(meta->read_long(), -1);
    EXPECT_EQ(meta->read_long(), 0);
    EXPECT_EQ(meta->read_short(), -1);
    EXPECT_EQ(meta->read_byte(), static_cast<uint8_t>(-1));
    EXPECT_EQ(meta->read_long(), 3);   // numValues
    EXPECT_EQ(meta->read_int(), -1);   // neither a unique-values table nor blocks
    EXPECT_EQ(meta->read_byte(), 2);   // ordinals 0..2 need two bits
    EXPECT_EQ(meta->read_long(), 0);   // ordinals start at 0
    EXPECT_EQ(meta->read_long(), 1);   // gcd of ordinals
    EXPECT_GT(meta->read_long(), 0);   // ordinals offset
    EXPECT_GT(meta->read_long(), 0);   // ordinals length
    EXPECT_EQ(meta->read_long(), -1);  // no block jump table

    // Term dictionary: the size, the monotonic block shift, the block addresses,
    // the maximum term length, the two ranges of the compressed terms and the
    // two ranges of the term addresses.
    EXPECT_EQ(meta->read_vlong(), 3);
    EXPECT_EQ(meta->read_int(), Lucene90DocValuesFormat::DIRECT_MONOTONIC_BLOCK_SHIFT);
    read_monotonic_meta(*meta);
    EXPECT_EQ(meta->read_int(), 6);   // max term length: "banana"
    EXPECT_GE(meta->read_int(), 0);   // max block length
    EXPECT_GT(meta->read_long(), 0);  // dictionary offset
    EXPECT_GT(meta->read_long(), 0);  // dictionary length
    EXPECT_GT(meta->read_long(), 0);  // addresses offset
    // Three terms fit in a single block, so every block address is 0 and the
    // monotonic encoding stores no data at all.
    EXPECT_EQ(meta->read_long(), 0);  // addresses length

    // Reverse index: the shift, one block of addresses and the two ranges.
    EXPECT_EQ(meta->read_int(), Lucene90DocValuesFormat::TERMS_DICT_REVERSE_INDEX_SHIFT);
    read_monotonic_meta(*meta);
    EXPECT_GT(meta->read_long(), 0);   // index offset
    EXPECT_EQ(meta->read_long(), 0);   // index length: a single block of terms needs no keys
    EXPECT_GT(meta->read_long(), 0);   // block addresses offset
    EXPECT_EQ(meta->read_long(), 0);   // block addresses length
    EXPECT_EQ(meta->read_int(), -1);   // end-of-fields marker
}

TEST(Lucene90DocValuesConsumerTest, ClosesIdempotently) {
    ByteBuffersDirectory directory;
    auto field_infos = std::make_shared<const FieldInfos>(
            FieldInfos::of({doc_values_field(0, "num", DocValuesType::Numeric)}).value());
    SegmentInfo segment_info = make_segment(1);
    SegmentWriteState state(directory, field_infos, io_context_default());

    auto consumer = Lucene90DocValuesConsumer::create(state, segment_info);
    ASSERT_TRUE(consumer.has_value());
    EXPECT_TRUE(consumer.value()->close().has_value());
    EXPECT_TRUE(consumer.value()->close().has_value());
}
