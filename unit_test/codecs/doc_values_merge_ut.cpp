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

module lucene_core:ut.doc_values_merge;

import lucene_core;

using namespace lucene;

namespace {

/// Sorted ords plus their dictionary, as a memory producer serves it.
struct SortedData {
    std::vector<int32_t> ords;
    std::vector<BytesRef> terms;
};

/// Sorted-set ords (one list per document) plus their dictionary.
struct SortedSetData {
    std::vector<std::vector<int64_t>> ords;
    std::vector<BytesRef> terms;
};

/// In-memory [`DocValuesProducer`] used both to write the source segments and to
/// describe their expected content.
class MemoryDocValuesProducer final : public DocValuesProducer {
public:
    void set_numeric(const std::string &field, std::vector<int64_t> values) {
        numeric_.insert_or_assign(field, std::move(values));
    }
    void set_binary(const std::string &field, std::vector<BytesRef> values) {
        binary_.insert_or_assign(field, std::move(values));
    }
    void set_sorted(const std::string &field, SortedData data) { sorted_.insert_or_assign(field, std::move(data)); }
    void set_sorted_numeric(const std::string &field, std::vector<std::vector<int64_t>> values) {
        sorted_numeric_.insert_or_assign(field, std::move(values));
    }
    void set_sorted_set(const std::string &field, SortedSetData data) {
        sorted_set_.insert_or_assign(field, std::move(data));
    }

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric(const FieldInfo &field) const override {
        auto found = numeric_.find(field.name);
        if (found == numeric_.end()) {
            return err(LuceneError::illegal_state("no numeric values for " + field.name));
        }
        return std::unique_ptr<NumericDocValues>(new InMemoryNumericDocValues(found->second));
    }

    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> get_binary(const FieldInfo &field) const override {
        auto found = binary_.find(field.name);
        if (found == binary_.end()) {
            return err(LuceneError::illegal_state("no binary values for " + field.name));
        }
        return std::unique_ptr<BinaryDocValues>(new InMemoryBinaryDocValues(found->second));
    }

    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted(const FieldInfo &field) const override {
        auto found = sorted_.find(field.name);
        if (found == sorted_.end()) {
            return err(LuceneError::illegal_state("no sorted values for " + field.name));
        }
        return std::unique_ptr<SortedDocValues>(
                new InMemorySortedDocValues(found->second.ords, found->second.terms));
    }

    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>> get_sorted_numeric(
            const FieldInfo &field) const override {
        auto found = sorted_numeric_.find(field.name);
        if (found == sorted_numeric_.end()) {
            return err(LuceneError::illegal_state("no sorted numeric values for " + field.name));
        }
        return std::unique_ptr<SortedNumericDocValues>(
                new InMemorySortedNumericDocValues(found->second));
    }

    [[nodiscard]] Result<std::unique_ptr<SortedSetDocValues>> get_sorted_set(const FieldInfo &field) const override {
        auto found = sorted_set_.find(field.name);
        if (found == sorted_set_.end()) {
            return err(LuceneError::illegal_state("no sorted set values for " + field.name));
        }
        return std::unique_ptr<SortedSetDocValues>(
                new InMemorySortedSetDocValues(found->second.ords, found->second.terms));
    }

    [[nodiscard]] VoidResult check_integrity() const override { return {}; }

    /// Whether this producer has values for `field`, i.e. whether `write_segment`
    /// should write that field at all.
    [[nodiscard]] bool has(const FieldInfo &field) const {
        switch (field.doc_values_type) {
            case DocValuesType::Numeric:
                return numeric_.contains(field.name);
            case DocValuesType::Binary:
                return binary_.contains(field.name);
            case DocValuesType::Sorted:
                return sorted_.contains(field.name);
            case DocValuesType::SortedNumeric:
                return sorted_numeric_.contains(field.name);
            case DocValuesType::SortedSet:
                return sorted_set_.contains(field.name);
            default:
                return false;
        }
    }

private:
    std::map<std::string, std::vector<int64_t>> numeric_;
    std::map<std::string, std::vector<BytesRef>> binary_;
    std::map<std::string, SortedData> sorted_;
    std::map<std::string, std::vector<std::vector<int64_t>>> sorted_numeric_;
    std::map<std::string, SortedSetData> sorted_set_;
};

FieldInfo dv_field(int32_t number, std::string name, DocValuesType type) {
    FieldInfo info;
    info.number = number;
    info.name = std::move(name);
    info.index_options = IndexOptions::None;
    info.doc_values_type = type;
    return info;
}

/// Five fields, one per doc values flavour.
std::shared_ptr<const FieldInfos> all_infos() {
    return std::make_shared<const FieldInfos>(FieldInfos::of({dv_field(0, "num", DocValuesType::Numeric),
                                                              dv_field(1, "bin", DocValuesType::Binary),
                                                              dv_field(2, "srt", DocValuesType::Sorted),
                                                              dv_field(3, "sn", DocValuesType::SortedNumeric),
                                                              dv_field(4, "ss", DocValuesType::SortedSet)})
                                                      .value());
}

SegmentInfo segment(std::string name, int32_t max_doc) {
    SegmentInfo info;
    info.name = std::move(name);
    info.id.assign(ID_LENGTH, 0x44);
    info.max_doc = max_doc;
    return info;
}

/// Writes one segment's doc values, one field per flavour that `producer` holds.
void write_segment(ByteBuffersDirectory &directory, const std::string &name, int32_t max_doc,
                   MemoryDocValuesProducer &producer, std::shared_ptr<const FieldInfos> infos = all_infos()) {
    SegmentInfo write_info = segment(name, max_doc);
    SegmentWriteState write_state(directory, infos, io_context_default());
    auto consumer = Lucene90DocValuesConsumer::create(write_state, write_info);
    ASSERT_TRUE(consumer.has_value()) << (consumer ? "" : consumer.error().to_string());
    for (const FieldInfo &field : infos->fields()) {
        if (!producer.has(field)) {
            continue;
        }
        VoidResult added = {};
        switch (field.doc_values_type) {
            case DocValuesType::Numeric:
                added = consumer.value()->add_numeric_field(field, producer);
                break;
            case DocValuesType::Binary:
                added = consumer.value()->add_binary_field(field, producer);
                break;
            case DocValuesType::Sorted:
                added = consumer.value()->add_sorted_field(field, producer);
                break;
            case DocValuesType::SortedNumeric:
                added = consumer.value()->add_sorted_numeric_field(field, producer);
                break;
            case DocValuesType::SortedSet:
                added = consumer.value()->add_sorted_set_field(field, producer);
                break;
            default:
                break;
        }
        ASSERT_TRUE(added.has_value()) << added.error().to_string();
    }
    ASSERT_TRUE(consumer.value()->close().has_value());
}

/// Opens the doc values of a segment written by [`write_segment`].
Result<std::unique_ptr<Lucene90DocValuesProducer>> open_producer(const SegmentReadState &state,
                                                                const SegmentInfo &segment_info) {
    return Lucene90DocValuesProducer::create(state, segment_info, Lucene90DocValuesFormat::DATA_CODEC,
                                             Lucene90DocValuesFormat::DATA_EXTENSION,
                                             Lucene90DocValuesFormat::META_CODEC,
                                             Lucene90DocValuesFormat::META_EXTENSION);
}

/// Minimal [`CodecReader`] exposing only a doc values producer.
class DocValuesCodecReader final : public CodecReader {
public:
    DocValuesCodecReader(DocValuesProducer *values, int32_t max_doc, int32_t num_docs,
                         std::shared_ptr<const Bits> live, std::shared_ptr<const FieldInfos> infos)
            : values_(values), max_doc_(max_doc), num_docs_(num_docs), live_(std::move(live)),
              infos_(std::move(infos)) {}

    [[nodiscard]] StoredFieldsReader *get_fields_reader() override { return nullptr; }
    [[nodiscard]] TermVectorsReader *get_term_vectors_reader() override { return nullptr; }
    [[nodiscard]] NormsProducer *get_norms_reader() override { return nullptr; }
    [[nodiscard]] DocValuesProducer *get_doc_values_reader() override { return values_; }
    [[nodiscard]] FieldsProducer *get_postings_reader() override { return nullptr; }
    [[nodiscard]] PointsReader *get_points_reader() override { return nullptr; }

    [[nodiscard]] Result<int32_t> max_doc() const override { return max_doc_; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return num_docs_; }
    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return infos_.get(); }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return live_.get(); }

private:
    DocValuesProducer *values_;
    int32_t max_doc_;
    int32_t num_docs_;
    std::shared_ptr<const Bits> live_;
    std::shared_ptr<const FieldInfos> infos_;
};

/// Collects `(doc, long)` pairs of a numeric field.
std::vector<std::pair<int32_t, int64_t>> read_numeric(DocValuesProducer &producer, const FieldInfo &field) {
    std::vector<std::pair<int32_t, int64_t>> result;
    auto values = producer.get_numeric(field);
    EXPECT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
    if (!values.has_value()) {
        return result;
    }
    while (true) {
        auto doc = values.value()->next_doc();
        EXPECT_TRUE(doc.has_value()) << (doc ? "" : doc.error().to_string());
        if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
            break;
        }
        auto value = values.value()->long_value();
        EXPECT_TRUE(value.has_value());
        result.emplace_back(doc.value(), value.value());
    }
    return result;
}

} // namespace

TEST(DocValuesMergeTest, MergesEveryDocValuesType) {
    ByteBuffersDirectory directory;

    // Segment _0: three documents.
    MemoryDocValuesProducer first_source;
    first_source.set_numeric("num", {10, 20, 30});
    first_source.set_binary("bin", {BytesRef::from_string("b0"), BytesRef::from_string("b1"),
                                    BytesRef::from_string("b2")});
    first_source.set_sorted("srt", SortedData{{0, 1, 0}, {BytesRef::from_string("a"), BytesRef::from_string("c")}});
    first_source.set_sorted_numeric("sn", {{1, 2}, {3}, {4, 5}});
    first_source.set_sorted_set("ss", SortedSetData{{{0, 1}, {1}, {0}},
                                                 {BytesRef::from_string("x"), BytesRef::from_string("y")}});
    write_segment(directory, "_0", 3, first_source);

    // Segment _1: two documents, the first one deleted.
    MemoryDocValuesProducer second_source;
    second_source.set_numeric("num", {40, 50});
    second_source.set_binary("bin", {BytesRef::from_string("b3"), BytesRef::from_string("b4")});
    second_source.set_sorted("srt", SortedData{{0, 0}, {BytesRef::from_string("b")}});
    second_source.set_sorted_numeric("sn", {{7}, {8, 9}});
    second_source.set_sorted_set("ss", SortedSetData{{{0}, {1}},
                                                  {BytesRef::from_string("y"), BytesRef::from_string("z")}});
    write_segment(directory, "_1", 2, second_source);

    auto infos = all_infos();
    SegmentReadState read_state(directory, infos, io_context_default());
    SegmentInfo read_info_a = segment("_0", 3);
    auto producer_a = open_producer(read_state, read_info_a);
    ASSERT_TRUE(producer_a.has_value()) << (producer_a ? "" : producer_a.error().to_string());
    SegmentInfo read_info_b = segment("_1", 2);
    auto producer_b = open_producer(read_state, read_info_b);
    ASSERT_TRUE(producer_b.has_value()) << (producer_b ? "" : producer_b.error().to_string());

    auto live = std::make_shared<FixedBitSet>(2);
    live->set(1);

    DocValuesCodecReader reader_a(producer_a.value().get(), 3, 3, nullptr, infos);
    DocValuesCodecReader reader_b(producer_b.value().get(), 2, 1, live, infos);
    std::vector<CodecReader *> readers{&reader_a, &reader_b};

    SegmentInfo merged_info = segment("_2", 0);
    auto merge_state = MergeState::create(readers, merged_info, std::make_shared<InfoStreamEnum>());
    ASSERT_TRUE(merge_state.has_value()) << merge_state.error().to_string();
    merge_state.value()->merge_field_infos = infos;
    ASSERT_EQ(merge_state.value()->segment_info.max_doc, 4);

    SegmentInfo write_info = segment("_2", 0);
    SegmentWriteState write_state(directory, infos, io_context_default());
    auto consumer = Lucene90DocValuesConsumer::create(write_state, write_info);
    ASSERT_TRUE(consumer.has_value()) << (consumer ? "" : consumer.error().to_string());
    auto merged = consumer.value()->merge(*merge_state.value());
    ASSERT_TRUE(merged.has_value()) << merged.error().to_string();
    ASSERT_TRUE(consumer.value()->close().has_value());

    SegmentInfo merged_read_info = segment("_2", 4);
    auto producer = open_producer(read_state, merged_read_info);
    ASSERT_TRUE(producer.has_value()) << (producer ? "" : producer.error().to_string());
    ASSERT_TRUE(producer.value()->check_integrity().has_value());

    const FieldInfo &num = *infos->by_name("num");
    const FieldInfo &bin = *infos->by_name("bin");
    const FieldInfo &srt = *infos->by_name("srt");
    const FieldInfo &sn = *infos->by_name("sn");
    const FieldInfo &ss = *infos->by_name("ss");

    // numeric: doc 3 comes from the second segment (its doc 0 was deleted).
    EXPECT_EQ(read_numeric(*producer.value(), num),
              (std::vector<std::pair<int32_t, int64_t>>{{0, 10}, {1, 20}, {2, 30}, {3, 50}}));

    // binary
    auto bin_values = producer.value()->get_binary(bin);
    ASSERT_TRUE(bin_values.has_value());
    std::vector<std::string> binaries;
    while (true) {
        auto doc = bin_values.value()->next_doc();
        ASSERT_TRUE(doc.has_value());
        if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
            break;
        }
        auto value = bin_values.value()->binary_value();
        ASSERT_TRUE(value.has_value());
        binaries.emplace_back(value.value().utf8_to_string().value());
    }
    EXPECT_EQ(binaries, (std::vector<std::string>{"b0", "b1", "b2", "b4"}));

    // sorted: {a, c, d} is the merged dictionary, docs map to it.
    auto sorted = producer.value()->get_sorted(srt);
    ASSERT_TRUE(sorted.has_value());
    auto term_count = sorted.value()->get_value_count();
    ASSERT_TRUE(term_count.has_value());
    ASSERT_EQ(term_count.value(), 3);
    std::vector<std::string> dictionary;
    for (int32_t ord = 0; ord < term_count.value(); ++ord) {
        auto term = sorted.value()->lookup_ord(ord);
        ASSERT_TRUE(term.has_value());
        dictionary.push_back(term.value().utf8_to_string().value());
    }
    EXPECT_EQ(dictionary, (std::vector<std::string>{"a", "b", "c"}));
    std::vector<std::string> sorted_values;
    while (true) {
        auto doc = sorted.value()->next_doc();
        ASSERT_TRUE(doc.has_value());
        if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
            break;
        }
        auto ord = sorted.value()->ord_value();
        ASSERT_TRUE(ord.has_value());
        auto term = sorted.value()->lookup_ord(ord.value());
        ASSERT_TRUE(term.has_value());
        sorted_values.push_back(term.value().utf8_to_string().value());
    }
    EXPECT_EQ(sorted_values, (std::vector<std::string>{"a", "c", "a", "b"}));

    // sorted numeric: {4,5} and {8,9} stay multi-valued, so the field is not
    // single-valued.
    auto sorted_numeric = producer.value()->get_sorted_numeric(sn);
    ASSERT_TRUE(sorted_numeric.has_value());
    EXPECT_FALSE(sorted_numeric.value()->is_single_valued());
    std::vector<std::pair<int32_t, std::vector<int64_t>>> numeric_lists;
    while (true) {
        auto doc = sorted_numeric.value()->next_doc();
        ASSERT_TRUE(doc.has_value());
        if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
            break;
        }
        auto count = sorted_numeric.value()->doc_value_count();
        ASSERT_TRUE(count.has_value());
        std::vector<int64_t> values;
        for (int32_t i = 0; i < count.value(); ++i) {
            auto value = sorted_numeric.value()->next_value();
            ASSERT_TRUE(value.has_value());
            values.push_back(value.value());
        }
        numeric_lists.emplace_back(doc.value(), std::move(values));
    }
    EXPECT_EQ(numeric_lists, (std::vector<std::pair<int32_t, std::vector<int64_t>>>{{0, {1, 2}},
                                                                                  {1, {3}},
                                                                                  {2, {4, 5}},
                                                                                  {3, {8, 9}}}));

    // sorted set: the merged dictionary is {x, y, z}
    auto sorted_set = producer.value()->get_sorted_set(ss);
    ASSERT_TRUE(sorted_set.has_value());
    auto ss_count = sorted_set.value()->get_value_count();
    ASSERT_TRUE(ss_count.has_value());
    ASSERT_EQ(ss_count.value(), 3);
    std::vector<std::string> ss_dictionary;
    for (int64_t ord = 0; ord < ss_count.value(); ++ord) {
        auto term = sorted_set.value()->lookup_ord(ord);
        ASSERT_TRUE(term.has_value());
        ss_dictionary.push_back(term.value().utf8_to_string().value());
    }
    EXPECT_EQ(ss_dictionary, (std::vector<std::string>{"x", "y", "z"}));
    std::vector<std::pair<int32_t, std::vector<std::string>>> set_values;
    while (true) {
        auto doc = sorted_set.value()->next_doc();
        ASSERT_TRUE(doc.has_value());
        if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
            break;
        }
        auto count = sorted_set.value()->doc_value_count();
        ASSERT_TRUE(count.has_value());
        std::vector<std::string> terms;
        for (int32_t i = 0; i < count.value(); ++i) {
            auto ord = sorted_set.value()->next_ord();
            ASSERT_TRUE(ord.has_value());
            auto term = sorted_set.value()->lookup_ord(ord.value());
            ASSERT_TRUE(term.has_value());
            terms.push_back(term.value().utf8_to_string().value());
        }
        set_values.emplace_back(doc.value(), std::move(terms));
    }
    EXPECT_EQ(set_values, (std::vector<std::pair<int32_t, std::vector<std::string>>>{{0, {"x", "y"}},
                                                                                    {1, {"y"}},
                                                                                    {2, {"x"}},
                                                                                    {3, {"z"}}}));
}

TEST(DocValuesMergeTest, EmptyMergeWritesNoDocValues) {
    ByteBuffersDirectory directory;

    MemoryDocValuesProducer source;
    source.set_numeric("num", {1, 2});
    auto infos = std::make_shared<const FieldInfos>(
            FieldInfos::of({dv_field(0, "num", DocValuesType::Numeric)}).value());
    write_segment(directory, "_0", 2, source, infos);

    SegmentReadState read_state(directory, infos, io_context_default());
    SegmentInfo read_info = segment("_0", 2);
    auto producer = open_producer(read_state, read_info);
    ASSERT_TRUE(producer.has_value()) << (producer ? "" : producer.error().to_string());

    auto live = std::make_shared<FixedBitSet>(2); // both deleted
    DocValuesCodecReader reader(producer.value().get(), 2, 0, live, infos);
    std::vector<CodecReader *> readers{&reader};

    SegmentInfo merged_info = segment("_1", 0);
    auto merge_state = MergeState::create(readers, merged_info, std::make_shared<InfoStreamEnum>());
    ASSERT_TRUE(merge_state.has_value());
    merge_state.value()->merge_field_infos = infos;
    ASSERT_EQ(merge_state.value()->segment_info.max_doc, 0);

    SegmentInfo write_info = segment("_1", 0);
    SegmentWriteState write_state(directory, infos, io_context_default());
    auto consumer = Lucene90DocValuesConsumer::create(write_state, write_info);
    ASSERT_TRUE(consumer.has_value());
    auto empty_merged = consumer.value()->merge(*merge_state.value());
    ASSERT_TRUE(empty_merged.has_value()) << empty_merged.error().to_string();
    ASSERT_TRUE(consumer.value()->close().has_value());

    // The merged segment still has all five fields, but no values.
    SegmentInfo merged_read_info = segment("_1", 0);
    auto merged = open_producer(read_state, merged_read_info);
    ASSERT_TRUE(merged.has_value()) << (merged ? "" : merged.error().to_string());
    EXPECT_TRUE(read_numeric(*merged.value(), *infos->by_name("num")).empty());
}
