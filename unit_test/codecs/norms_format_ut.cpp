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
#include <string>
#include <utility>
#include <variant>
#include <vector>

module lucene_core:ut.norms_format;

import lucene_core;

using namespace lucene;

namespace {

class FixedNumericDocValues : public NumericDocValues {
public:
    explicit FixedNumericDocValues(int64_t value) : value_(value) {}

    [[nodiscard]] int32_t doc_id() const override { return 0; }
    [[nodiscard]] Result<int32_t> next_doc() override { return NO_MORE_DOCS; }
    [[nodiscard]] Result<int64_t> long_value() override { return value_; }

private:
    int64_t value_;
};

class FixedNormsProducer : public NormsProducer {
public:
    explicit FixedNormsProducer(int64_t value) : value_(value) {}

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norms(const FieldInfo & /*field*/) const override {
        return std::unique_ptr<NumericDocValues>(std::make_unique<FixedNumericDocValues>(value_));
    }

    [[nodiscard]] VoidResult check_integrity() const override { return {}; }

private:
    int64_t value_;
};

/// Pulls the norms of every added field and records them.
class RecordingNormsConsumer : public NormsConsumer {
public:
    [[nodiscard]] VoidResult add_norms_field(const FieldInfo &field, NormsProducer &norms_producer) override {
        auto values = norms_producer.get_norms(field);
        if (!values) {
            return err(std::move(values.error()));
        }
        auto value = values.value()->long_value();
        if (!value) {
            return err(std::move(value.error()));
        }
        fields.push_back(field.name);
        values_seen.push_back(value.value());
        return {};
    }

    [[nodiscard]] VoidResult close() override {
        closed = true;
        return {};
    }

    std::vector<std::string> fields;
    std::vector<int64_t> values_seen;
    bool closed{false};
};

class RecordingNormsFormat : public NormsFormat {
public:
    [[nodiscard]] Result<std::unique_ptr<NormsConsumer>> norms_consumer(const SegmentWriteState &state,
                                                                       const SegmentInfo & /*segment_info*/) const override {
        write_suffix = state.segment_suffix;
        return std::unique_ptr<NormsConsumer>(std::make_unique<RecordingNormsConsumer>());
    }

    [[nodiscard]] Result<std::unique_ptr<NormsProducer>> norms_producer(const SegmentReadState &state,
                                                                      const SegmentInfo & /*segment_info*/) const override {
        read_suffix = state.segment_suffix;
        return std::unique_ptr<NormsProducer>(std::make_unique<FixedNormsProducer>(42));
    }

    mutable std::string write_suffix;
    mutable std::string read_suffix;
};

FieldInfo norms_field() {
    FieldInfo info;
    info.name = "norms";
    info.index_options = IndexOptions::DocsAndFreqs;
    return info;
}

} // namespace

TEST(NormsConsumerEnumTest, DispatchesPerAlternative) {
    using Enum = NormsConsumerEnum<RecordingNormsConsumer, RecordingNormsConsumer>;

    Enum first{std::variant<RecordingNormsConsumer, RecordingNormsConsumer>{
            std::in_place_index<0>, RecordingNormsConsumer{}}};
    FixedNormsProducer producer{5};
    ASSERT_TRUE(first.add_norms_field(norms_field(), producer).has_value());
    ASSERT_TRUE(first.close().has_value());

    const auto &consumer = std::get<0>(first.value());
    ASSERT_EQ(consumer.fields.size(), 1u);
    EXPECT_EQ(consumer.fields[0], "norms");
    ASSERT_EQ(consumer.values_seen.size(), 1u);
    EXPECT_EQ(consumer.values_seen[0], 5);
    EXPECT_TRUE(consumer.closed);

    Enum second{std::variant<RecordingNormsConsumer, RecordingNormsConsumer>{
            std::in_place_index<1>, RecordingNormsConsumer{}}};
    ASSERT_TRUE(second.add_norms_field(norms_field(), producer).has_value());
    EXPECT_EQ(std::get<1>(second.value()).values_seen[0], 5);
}

TEST(NormsFormatTest, ForwardsTheSegmentSuffix) {
    ByteBuffersDirectory directory;
    const IOContext &context = io_context_default();
    auto field_infos = std::make_shared<const FieldInfos>(FieldInfos::none());
    RecordingNormsFormat format;
    SegmentInfo segment_info;

    SegmentWriteState write_state{get_default_info_stream(), directory, field_infos, context, "_3"};
    auto consumer = format.norms_consumer(write_state, segment_info);
    ASSERT_TRUE(consumer.has_value());
    EXPECT_EQ(format.write_suffix, "_3");

    FixedNormsProducer producer{7};
    ASSERT_TRUE(consumer.value()->add_norms_field(norms_field(), producer).has_value());
    ASSERT_TRUE(consumer.value()->close().has_value());

    SegmentReadState read_state{directory, field_infos, context, "_4"};
    auto read_producer = format.norms_producer(read_state, segment_info);
    ASSERT_TRUE(read_producer.has_value());
    EXPECT_EQ(format.read_suffix, "_4");

    auto values = read_producer.value()->get_norms(norms_field());
    ASSERT_TRUE(values.has_value());
    EXPECT_EQ(values.value()->long_value().value(), 42);
}
