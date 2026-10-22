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
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.norms_merge;

import lucene_core;

using namespace lucene;

namespace {

using Pairs = std::vector<std::pair<int32_t, int64_t>>;

/// Norms over an explicit `(doc, value)` list.
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
        while (true) {
            if (position_ >= pairs_.size()) {
                doc_ = NO_MORE_DOCS;
                return doc_;
            }
            if (pairs_[position_].first >= target) {
                doc_ = pairs_[position_++].first;
                return doc_;
            }
            ++position_;
        }
    }

    [[nodiscard]] Result<int32_t> slow_advance(int32_t target) override { return advance(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(pairs_.size()); }

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

/// In-memory source of norms for one field.
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

private:
    std::map<int32_t, Pairs> fields_;
};

std::shared_ptr<const FieldInfos> norms_infos() {
    FieldInfo field;
    field.number = 0;
    field.name = "f";
    field.index_options = IndexOptions::DocsAndFreqs;
    return std::make_shared<const FieldInfos>(FieldInfos::of({field}).value());
}

SegmentInfo segment(std::string name, int32_t max_doc) {
    SegmentInfo info;
    info.name = std::move(name);
    info.id.assign(ID_LENGTH, 0x33);
    info.max_doc = max_doc;
    return info;
}

/// Writes one segment's norms and returns the opened producer.
std::unique_ptr<NormsProducer> write_segment(ByteBuffersDirectory &directory, const std::string &name,
                                            const Pairs &pairs) {
    auto infos = norms_infos();
    SegmentInfo write_info = segment(name, 10);
    SegmentWriteState write_state(directory, infos, io_context_default());
    auto consumer = Lucene90NormsFormat{}.norms_consumer(write_state, write_info);
    EXPECT_TRUE(consumer.has_value()) << (consumer ? "" : consumer.error().to_string());
    if (!consumer.has_value()) {
        return nullptr;
    }
    PairsNormsProducer source;
    source.add(0, pairs);
    for (const FieldInfo &field : infos->fields()) {
        EXPECT_TRUE(consumer.value()->add_norms_field(field, source).has_value());
    }
    EXPECT_TRUE(consumer.value()->close().has_value());

    SegmentInfo read_info = segment(name, 10);
    SegmentReadState read_state(directory, infos, io_context_default());
    auto producer = Lucene90NormsFormat{}.norms_producer(read_state, read_info);
    EXPECT_TRUE(producer.has_value()) << (producer ? "" : producer.error().to_string());
    return producer.has_value() ? std::move(producer.value()) : nullptr;
}

Pairs read_all(NormsProducer &producer, const FieldInfo &field) {
    auto values = producer.get_norms(field);
    EXPECT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
    Pairs pairs;
    if (!values.has_value()) {
        return pairs;
    }
    while (true) {
        auto doc = values.value()->next_doc();
        EXPECT_TRUE(doc.has_value()) << (doc ? "" : doc.error().to_string());
        if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
            break;
        }
        auto value = values.value()->long_value();
        EXPECT_TRUE(value.has_value());
        pairs.emplace_back(doc.value(), value.value());
    }
    return pairs;
}

/// Minimal [`CodecReader`] exposing only a norms producer.
class NormsCodecReader final : public CodecReader {
public:
    NormsCodecReader(NormsProducer *norms, int32_t max_doc, int32_t num_docs, std::shared_ptr<const Bits> live,
                     std::shared_ptr<const FieldInfos> infos)
            : norms_(norms), max_doc_(max_doc), num_docs_(num_docs), live_(std::move(live)),
              infos_(std::move(infos)) {}

    [[nodiscard]] StoredFieldsReader *get_fields_reader() override { return nullptr; }
    [[nodiscard]] TermVectorsReader *get_term_vectors_reader() override { return nullptr; }
    [[nodiscard]] NormsProducer *get_norms_reader() override { return norms_; }
    [[nodiscard]] DocValuesProducer *get_doc_values_reader() override { return nullptr; }
    [[nodiscard]] FieldsProducer *get_postings_reader() override { return nullptr; }
    [[nodiscard]] PointsReader *get_points_reader() override { return nullptr; }

    [[nodiscard]] Result<int32_t> max_doc() const override { return max_doc_; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return num_docs_; }
    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return infos_.get(); }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return live_.get(); }

private:
    NormsProducer *norms_;
    int32_t max_doc_;
    int32_t num_docs_;
    std::shared_ptr<const Bits> live_;
    std::shared_ptr<const FieldInfos> infos_;
};

} // namespace

TEST(NormsMergeTest, ConcatenatesSegmentsAndSkipsDeletedDocuments) {
    ByteBuffersDirectory directory;

    auto first = write_segment(directory, "_0", Pairs{{0, 1}, {1, 2}, {2, 3}, {3, 4}});
    ASSERT_TRUE(first != nullptr);
    auto second = write_segment(directory, "_1", Pairs{{0, 10}, {1, 11}, {2, 12}});
    ASSERT_TRUE(second != nullptr);

    auto live = std::make_shared<FixedBitSet>(3);
    live->set(0);
    live->set(2);
    auto infos = norms_infos();

    NormsCodecReader reader_a(first.get(), 4, 4, nullptr, infos);
    NormsCodecReader reader_b(second.get(), 3, 2, live, infos);
    std::vector<CodecReader *> readers{&reader_a, &reader_b};

    SegmentInfo merged_info = segment("_2", 0);
    auto merge_state = MergeState::create(readers, merged_info, std::make_shared<InfoStreamEnum>());
    ASSERT_TRUE(merge_state.has_value()) << merge_state.error().to_string();
    merge_state.value()->merge_field_infos = infos;
    EXPECT_EQ(merge_state.value()->segment_info.max_doc, 6);

    SegmentInfo write_info = segment("_2", 0);
    SegmentWriteState write_state(directory, infos, io_context_default());
    auto consumer = Lucene90NormsFormat{}.norms_consumer(write_state, write_info);
    ASSERT_TRUE(consumer.has_value()) << (consumer ? "" : consumer.error().to_string());
    auto merged = consumer.value()->merge(*merge_state.value());
    ASSERT_TRUE(merged.has_value()) << merged.error().to_string();
    ASSERT_TRUE(consumer.value()->close().has_value());

    SegmentInfo read_info = segment("_2", 6);
    SegmentReadState read_state(directory, infos, io_context_default());
    auto producer = Lucene90NormsFormat{}.norms_producer(read_state, read_info);
    ASSERT_TRUE(producer.has_value()) << (producer ? "" : producer.error().to_string());
    ASSERT_TRUE(producer.value()->check_integrity().has_value());

    // Documents 4 and 5 come from the second segment (doc 1 there is deleted).
    EXPECT_EQ(read_all(*producer.value(), infos->fields()[0]), (Pairs{{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 10}, {5, 12}}));
}

TEST(NormsMergeTest, SingleSegmentWithNoDeletionsIsCopied) {
    ByteBuffersDirectory directory;

    auto only = write_segment(directory, "_0", Pairs{{1, 7}, {3, -2}});
    ASSERT_TRUE(only != nullptr);
    auto infos = norms_infos();

    NormsCodecReader reader(only.get(), 4, 4, nullptr, infos);
    std::vector<CodecReader *> readers{&reader};

    SegmentInfo merged_info = segment("_1", 0);
    auto merge_state = MergeState::create(readers, merged_info, std::make_shared<InfoStreamEnum>());
    ASSERT_TRUE(merge_state.has_value());
    merge_state.value()->merge_field_infos = infos;

    SegmentInfo write_info = segment("_1", 0);
    SegmentWriteState write_state(directory, infos, io_context_default());
    auto consumer = Lucene90NormsFormat{}.norms_consumer(write_state, write_info);
    ASSERT_TRUE(consumer.has_value());
    ASSERT_TRUE(consumer.value()->merge(*merge_state.value()).has_value());
    ASSERT_TRUE(consumer.value()->close().has_value());

    SegmentInfo read_info = segment("_1", 4);
    SegmentReadState read_state(directory, infos, io_context_default());
    auto producer = Lucene90NormsFormat{}.norms_producer(read_state, read_info);
    ASSERT_TRUE(producer.has_value());
    EXPECT_EQ(read_all(*producer.value(), infos->fields()[0]), (Pairs{{1, 7}, {3, -2}}));
}
