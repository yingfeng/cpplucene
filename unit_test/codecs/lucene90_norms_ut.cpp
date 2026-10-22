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

module lucene_core:ut.lucene90_norms;

import lucene_core;

using namespace lucene;

namespace {

using Pairs = std::vector<std::pair<int32_t, int64_t>>;

/// Norms of one field, over an explicit list of `(doc, value)` pairs.
class PairsNormsValues final : public NumericDocValues {
public:
    explicit PairsNormsValues(Pairs pairs) : pairs_(std::move(pairs)) {}

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

private:
    Pairs pairs_;
    size_t position_{0};
    int32_t doc_{-1};
};

/// A [`NormsProducer`] over in-memory pairs, i.e. what the flush path feeds into
/// the norms consumer.
class PairsNormsProducer final : public NormsProducer {
public:
    void add(int32_t field_number, Pairs pairs) { fields_.insert_or_assign(field_number, std::move(pairs)); }

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norms(const FieldInfo &field) const override {
        auto found = fields_.find(field.number);
        if (found == fields_.end()) {
            return err(LuceneError::illegal_state("no norms for field " + field.name));
        }
        return std::unique_ptr<NumericDocValues>(new PairsNormsValues(found->second));
    }

    [[nodiscard]] VoidResult check_integrity() const override { return {}; }

    [[nodiscard]] VoidResult close() const override { return {}; }

private:
    std::map<int32_t, Pairs> fields_;
};

FieldInfo norms_field(int32_t number, std::string name) {
    FieldInfo field;
    field.number = number;
    field.name = std::move(name);
    field.index_options = IndexOptions::DocsAndFreqs;
    return field;
}

SegmentInfo make_segment(int32_t max_doc, std::string name = "_0") {
    SegmentInfo info;
    info.name = std::move(name);
    info.id.assign(ID_LENGTH, 0x33);
    info.max_doc = max_doc;
    return info;
}

/// Reads a whole field back into pairs.
Pairs read_all(NormsProducer &producer, const FieldInfo &field) {
    auto values = producer.get_norms(field);
    EXPECT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
    Pairs pairs;
    while (true) {
        auto doc = values.value()->next_doc();
        EXPECT_TRUE(doc.has_value()) << (doc ? "" : doc.error().to_string());
        if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
            break;
        }
        auto value = values.value()->long_value();
        EXPECT_TRUE(value.has_value()) << (value ? "" : value.error().to_string());
        pairs.emplace_back(doc.value(), value.value());
    }
    return pairs;
}

struct RoundTrip {
    ByteBuffersDirectory directory;
    std::shared_ptr<const FieldInfos> field_infos;
    SegmentInfo segment_info;
};

/// Writes `pairs` with the consumer and reopens them with the producer.
void write_norms(RoundTrip &round_trip, PairsNormsProducer &source) {
    SegmentWriteState write_state(round_trip.directory, round_trip.field_infos, io_context_default());
    auto consumer = Lucene90NormsFormat{}.norms_consumer(write_state, round_trip.segment_info);
    ASSERT_TRUE(consumer.has_value()) << (consumer ? "" : consumer.error().to_string());
    for (const FieldInfo &field : round_trip.field_infos->fields()) {
        ASSERT_TRUE(consumer.value()->add_norms_field(field, source).has_value());
    }
    ASSERT_TRUE(consumer.value()->close().has_value());
}

} // namespace

TEST(Lucene90NormsTest, RoundTripsASparseField) {
    RoundTrip round_trip;
    round_trip.field_infos = std::make_shared<const FieldInfos>(FieldInfos::of({norms_field(0, "f")}).value());
    round_trip.segment_info = make_segment(100);

    PairsNormsProducer source;
    // A range that straddles zero cannot use the narrower widths.
    const Pairs pairs{{0, 1}, {5, 2}, {50, 300}, {99, -7}};
    source.add(0, pairs);
    write_norms(round_trip, source);

    SegmentReadState read_state(round_trip.directory, round_trip.field_infos, io_context_default());
    auto producer = Lucene90NormsFormat{}.norms_producer(read_state, round_trip.segment_info);
    ASSERT_TRUE(producer.has_value()) << (producer ? "" : producer.error().to_string());
    ASSERT_TRUE(producer.value()->check_integrity().has_value());

    const FieldInfo &field = round_trip.field_infos->fields()[0];
    EXPECT_EQ(read_all(*producer.value(), field), pairs);

    // A document without a norm reports no value, and the cursor still lands on it.
    auto values = producer.value()->get_norms(field);
    ASSERT_TRUE(values.has_value());
    auto missing = values.value()->advance_exact(7);
    ASSERT_TRUE(missing.has_value());
    EXPECT_FALSE(missing.value());
    EXPECT_EQ(values.value()->doc_id(), 7);

    auto present = values.value()->advance_exact(50);
    ASSERT_TRUE(present.has_value());
    EXPECT_TRUE(present.value());
    EXPECT_EQ(values.value()->doc_id(), 50);
    EXPECT_EQ(values.value()->long_value().value(), 300);

    // Skipping ahead lands on the next document that has a norm.
    auto skipped = values.value()->advance(51);
    ASSERT_TRUE(skipped.has_value());
    EXPECT_EQ(skipped.value(), 99);
    EXPECT_EQ(values.value()->long_value().value(), -7);
}

TEST(Lucene90NormsTest, RoundTripsADenseField) {
    RoundTrip round_trip;
    round_trip.field_infos = std::make_shared<const FieldInfos>(FieldInfos::of({norms_field(0, "f")}).value());
    round_trip.segment_info = make_segment(50);

    PairsNormsProducer source;
    Pairs pairs;
    for (int32_t doc = 0; doc < 50; ++doc) {
        // Fits in one byte, so the values are written with a width of 1.
        pairs.emplace_back(doc, doc - 3);
    }
    source.add(0, pairs);
    write_norms(round_trip, source);

    SegmentReadState read_state(round_trip.directory, round_trip.field_infos, io_context_default());
    auto producer = Lucene90NormsFormat{}.norms_producer(read_state, round_trip.segment_info);
    ASSERT_TRUE(producer.has_value());

    const FieldInfo &field = round_trip.field_infos->fields()[0];
    EXPECT_EQ(read_all(*producer.value(), field), pairs);

    // Every document has a value, so `advance_exact` always reports one.
    auto values = producer.value()->get_norms(field);
    ASSERT_TRUE(values.has_value());
    auto exact = values.value()->advance_exact(37);
    ASSERT_TRUE(exact.has_value());
    EXPECT_TRUE(exact.value());
    EXPECT_EQ(values.value()->long_value().value(), 34);
    EXPECT_EQ(values.value()->cost().value(), 50);

    auto past_the_end = values.value()->advance(50);
    ASSERT_TRUE(past_the_end.has_value());
    EXPECT_EQ(past_the_end.value(), NO_MORE_DOCS);
}

TEST(Lucene90NormsTest, RoundTripsAConstantField) {
    RoundTrip round_trip;
    round_trip.field_infos = std::make_shared<const FieldInfos>(FieldInfos::of({norms_field(0, "f")}).value());
    round_trip.segment_info = make_segment(10);

    PairsNormsProducer source;
    Pairs pairs;
    for (int32_t doc = 0; doc < 10; ++doc) {
        pairs.emplace_back(doc, 42);
    }
    source.add(0, pairs);
    write_norms(round_trip, source);

    SegmentReadState read_state(round_trip.directory, round_trip.field_infos, io_context_default());
    auto producer = Lucene90NormsFormat{}.norms_producer(read_state, round_trip.segment_info);
    ASSERT_TRUE(producer.has_value());

    // A single value needs no bytes in the data file: it is stored in the metadata.
    EXPECT_EQ(read_all(*producer.value(), round_trip.field_infos->fields()[0]), pairs);
}

TEST(Lucene90NormsTest, RoundTripsASparseConstantField) {
    RoundTrip round_trip;
    round_trip.field_infos = std::make_shared<const FieldInfos>(FieldInfos::of({norms_field(0, "f")}).value());
    round_trip.segment_info = make_segment(20);

    PairsNormsProducer source;
    // Same value for a subset of the documents: the doc-id set still has to be
    // written, but the values collapse to the constant.
    const Pairs pairs{{1, 5}, {4, 5}, {19, 5}};
    source.add(0, pairs);
    write_norms(round_trip, source);

    SegmentReadState read_state(round_trip.directory, round_trip.field_infos, io_context_default());
    auto producer = Lucene90NormsFormat{}.norms_producer(read_state, round_trip.segment_info);
    ASSERT_TRUE(producer.has_value());
    EXPECT_EQ(read_all(*producer.value(), round_trip.field_infos->fields()[0]), pairs);
}

TEST(Lucene90NormsTest, RoundTripsAFieldWithoutValues) {
    RoundTrip round_trip;
    round_trip.field_infos = std::make_shared<const FieldInfos>(FieldInfos::of({norms_field(0, "f")}).value());
    round_trip.segment_info = make_segment(7);

    PairsNormsProducer source;
    source.add(0, Pairs{});
    write_norms(round_trip, source);

    SegmentReadState read_state(round_trip.directory, round_trip.field_infos, io_context_default());
    auto producer = Lucene90NormsFormat{}.norms_producer(read_state, round_trip.segment_info);
    ASSERT_TRUE(producer.has_value());

    auto values = producer.value()->get_norms(round_trip.field_infos->fields()[0]);
    ASSERT_TRUE(values.has_value());
    EXPECT_EQ(values.value()->next_doc().value(), NO_MORE_DOCS);
    EXPECT_EQ(values.value()->cost().value(), 0);
    // Asking for a value while no document is positioned fails cleanly.
    EXPECT_FALSE(values.value()->long_value().has_value());
}

TEST(Lucene90NormsTest, RoundTripsSeveralFields) {
    RoundTrip round_trip;
    round_trip.field_infos =
            std::make_shared<const FieldInfos>(FieldInfos::of({norms_field(0, "sparse"), norms_field(1, "dense")}).value());
    round_trip.segment_info = make_segment(8);

    PairsNormsProducer source;
    const Pairs sparse_pairs{{2, 1000}, {5, -1000}};
    Pairs dense_pairs;
    for (int32_t doc = 0; doc < 8; ++doc) {
        dense_pairs.emplace_back(doc, doc * 1000000LL);
    }
    source.add(0, sparse_pairs);
    source.add(1, dense_pairs);
    write_norms(round_trip, source);

    auto names = round_trip.directory.list_all();
    ASSERT_TRUE(names.has_value());
    EXPECT_NE(std::find(names->begin(), names->end(), "_0.nvd"), names->end());
    EXPECT_NE(std::find(names->begin(), names->end(), "_0.nvm"), names->end());

    SegmentReadState read_state(round_trip.directory, round_trip.field_infos, io_context_default());
    auto producer = Lucene90NormsFormat{}.norms_producer(read_state, round_trip.segment_info);
    ASSERT_TRUE(producer.has_value());
    EXPECT_EQ(read_all(*producer.value(), round_trip.field_infos->fields()[0]), sparse_pairs);
    EXPECT_EQ(read_all(*producer.value(), round_trip.field_infos->fields()[1]), dense_pairs);

    // A field that is not in the metadata is reported.
    FieldInfo unknown = norms_field(9, "missing");
    EXPECT_FALSE(producer.value()->get_norms(unknown).has_value());
}
