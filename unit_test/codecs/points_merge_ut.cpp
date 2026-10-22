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
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.points_merge;

import lucene_core;

using namespace lucene;

namespace {

constexpr size_t BYTES_PER_DIM = 4;

/// A single-leaf point source; the points writer only walks leaves.
class SourcePointValues;

class SourcePointTree final : public PointTree {
public:
    explicit SourcePointTree(const SourcePointValues &values) : values_(values) {}

    [[nodiscard]] Result<bool> move_to_child() override { return false; }
    [[nodiscard]] Result<bool> move_to_sibling() override { return false; }
    [[nodiscard]] Result<bool> move_to_parent() override { return false; }
    [[nodiscard]] Result<BytesRef> get_min_packed_value() override;
    [[nodiscard]] Result<BytesRef> get_max_packed_value() override;
    [[nodiscard]] Result<size_t> size() override;
    [[nodiscard]] VoidResult visit_doc_ids(IntersectVisitor &visitor) override;
    [[nodiscard]] VoidResult visit_doc_values(IntersectVisitor &visitor) override;
    [[nodiscard]] std::unique_ptr<PointTree> try_clone() const override;

private:
    const SourcePointValues &values_;
};

class SourcePointValues final : public PointValues {
public:
    SourcePointValues(std::vector<std::vector<uint8_t>> values, std::vector<int32_t> docs)
            : values_(std::move(values)), docs_(std::move(docs)) {}

    [[nodiscard]] Result<std::optional<BytesRef>> get_min_packed_value() const override {
        return std::optional<BytesRef>(BytesRef(values_.empty() ? std::vector<uint8_t>{0, 0, 0, 0} : values_.front()));
    }
    [[nodiscard]] Result<std::optional<BytesRef>> get_max_packed_value() const override {
        return std::optional<BytesRef>(BytesRef(values_.empty() ? std::vector<uint8_t>{0, 0, 0, 0} : values_.back()));
    }
    [[nodiscard]] Result<size_t> get_num_dimensions() const override { return 1; }
    [[nodiscard]] Result<size_t> get_num_index_dimensions() const override { return 1; }
    [[nodiscard]] Result<size_t> get_bytes_per_dimension() const override { return BYTES_PER_DIM; }
    [[nodiscard]] Result<size_t> size() const override { return values_.size(); }
    [[nodiscard]] Result<int32_t> get_doc_count() const override {
        return static_cast<int32_t>(docs_.size());
    }
    [[nodiscard]] Result<std::unique_ptr<PointTree>> get_point_tree() const override {
        return std::unique_ptr<PointTree>(new SourcePointTree(*this));
    }

    [[nodiscard]] const std::vector<std::vector<uint8_t>> &values() const noexcept { return values_; }
    [[nodiscard]] const std::vector<int32_t> &docs() const noexcept { return docs_; }

private:
    std::vector<std::vector<uint8_t>> values_;
    std::vector<int32_t> docs_;
};

Result<BytesRef> SourcePointTree::get_min_packed_value() {
    return BytesRef(values_.values().front());
}

Result<BytesRef> SourcePointTree::get_max_packed_value() {
    return BytesRef(values_.values().back());
}

Result<size_t> SourcePointTree::size() {
    return values_.values().size();
}

VoidResult SourcePointTree::visit_doc_ids(IntersectVisitor &visitor) {
    for (const int32_t doc : values_.docs()) {
        if (auto visited = visitor.visit(doc); !visited.has_value()) {
            return visited;
        }
    }
    return {};
}

VoidResult SourcePointTree::visit_doc_values(IntersectVisitor &visitor) {
    const auto &points = values_.values();
    const auto &docs = values_.docs();
    for (size_t i = 0; i < points.size(); ++i) {
        if (auto visited = visitor.visit_with_packed_value(docs[i], std::span<const uint8_t>(points[i]));
            !visited.has_value()) {
            return visited;
        }
    }
    return {};
}

std::unique_ptr<PointTree> SourcePointTree::try_clone() const {
    return std::unique_ptr<PointTree>(new SourcePointTree(values_));
}

/// A [`PointsReader`] over one field of a source segment.
class SourcePointsReader final : public PointsReader {
public:
    SourcePointsReader(std::string field_name, std::shared_ptr<PointValues> values)
            : field_name_(std::move(field_name)), values_(std::move(values)) {}

    [[nodiscard]] VoidResult check_integrity() override { return {}; }
    [[nodiscard]] VoidResult close() override { return {}; }

    [[nodiscard]] Result<std::shared_ptr<PointValues>> get_values(std::string_view field_name) const override {
        if (field_name != field_name_) {
            return err(LuceneError::illegal_state("unknown field"));
        }
        return values_;
    }

private:
    std::string field_name_;
    std::shared_ptr<PointValues> values_;
};

/// Collects every point a tree reports.
class CollectVisitor final : public IntersectVisitor {
public:
    [[nodiscard]] VoidResult visit(int32_t doc_id) override { return visit_with_packed_value(doc_id, {}); }

    [[nodiscard]] VoidResult visit_with_packed_value(int32_t doc_id, std::span<const uint8_t> packed_value) override {
        pairs.emplace_back(doc_id, BitUtil::get_i32_be(packed_value, 0));
        return {};
    }

    [[nodiscard]] Result<Relation> compare(std::span<const uint8_t> /*min*/,
                                           std::span<const uint8_t> /*max*/) const override {
        return Relation::kCellCrossesQuery;
    }

    std::vector<std::pair<int32_t, int32_t>> pairs;
};

FieldInfo point_field(int32_t number, std::string name) {
    FieldInfo field;
    field.number = number;
    field.name = std::move(name);
    field.point_dimension_count = 1;
    field.point_index_dimension_count = 1;
    field.point_num_bytes = static_cast<int32_t>(BYTES_PER_DIM);
    return field;
}

SegmentInfo segment(std::string name, int32_t max_doc) {
    SegmentInfo info;
    info.name = std::move(name);
    info.id.assign(ID_LENGTH, 0x5A);
    info.max_doc = max_doc;
    return info;
}

std::vector<uint8_t> pack(int32_t value) {
    std::vector<uint8_t> packed(BYTES_PER_DIM, 0);
    BitUtil::set_i32_be(packed, 0, value);
    return packed;
}

/// Writes one segment's points and returns the opened reader.
std::unique_ptr<PointsReader> write_segment(ByteBuffersDirectory &directory, const std::string &name,
                                            const std::vector<int32_t> &doc_values) {
    auto infos = std::make_shared<const FieldInfos>(FieldInfos::of({point_field(0, "pt")}).value());
    SegmentInfo write_info = segment(name, static_cast<int32_t>(doc_values.size()));
    SegmentWriteState write_state(directory, infos, io_context_default());

    std::vector<std::vector<uint8_t>> points;
    std::vector<int32_t> docs;
    for (size_t i = 0; i < doc_values.size(); ++i) {
        points.push_back(pack(doc_values[i]));
        docs.push_back(static_cast<int32_t>(i));
    }
    auto source = std::make_shared<SourcePointValues>(std::move(points), std::move(docs));
    SourcePointsReader source_reader("pt", source);

    auto writer = Lucene90PointsFormat::fields_writer(write_state, write_info);
    EXPECT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    if (!writer.has_value()) {
        return nullptr;
    }
    EXPECT_TRUE(writer.value()->write_field(infos->fields()[0], source_reader).has_value());
    EXPECT_TRUE(writer.value()->finish().has_value());
    EXPECT_TRUE(writer.value()->close().has_value());

    SegmentInfo read_info = segment(name, static_cast<int32_t>(doc_values.size()));
    SegmentReadState read_state(directory, infos, io_context_default());
    auto reader = Lucene90PointsFormat::fields_reader(read_state, read_info);
    EXPECT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    return reader.has_value() ? std::move(reader.value()) : nullptr;
}

/// Minimal [`CodecReader`] exposing only a points reader.
class PointsCodecReader final : public CodecReader {
public:
    PointsCodecReader(PointsReader *points, int32_t max_doc, int32_t num_docs, std::shared_ptr<const Bits> live,
                      std::shared_ptr<const FieldInfos> infos)
            : points_(points), max_doc_(max_doc), num_docs_(num_docs), live_(std::move(live)),
              infos_(std::move(infos)) {}

    [[nodiscard]] StoredFieldsReader *get_fields_reader() override { return nullptr; }
    [[nodiscard]] TermVectorsReader *get_term_vectors_reader() override { return nullptr; }
    [[nodiscard]] NormsProducer *get_norms_reader() override { return nullptr; }
    [[nodiscard]] DocValuesProducer *get_doc_values_reader() override { return nullptr; }
    [[nodiscard]] FieldsProducer *get_postings_reader() override { return nullptr; }
    [[nodiscard]] PointsReader *get_points_reader() override { return points_; }

    [[nodiscard]] Result<int32_t> max_doc() const override { return max_doc_; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return num_docs_; }
    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return infos_.get(); }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return live_.get(); }

private:
    PointsReader *points_;
    int32_t max_doc_;
    int32_t num_docs_;
    std::shared_ptr<const Bits> live_;
    std::shared_ptr<const FieldInfos> infos_;
};

} // namespace

TEST(PointsMergeTest, ConcatenatesSegmentsAndSkipsDeletedDocuments) {
    ByteBuffersDirectory directory;

    auto first = write_segment(directory, "_0", {10, 20, 30});
    ASSERT_TRUE(first != nullptr);
    auto second = write_segment(directory, "_1", {40, 50});
    ASSERT_TRUE(second != nullptr);

    auto live = std::make_shared<FixedBitSet>(2);
    live->set(0);
    auto infos = std::make_shared<const FieldInfos>(FieldInfos::of({point_field(0, "pt")}).value());

    PointsCodecReader reader_a(first.get(), 3, 3, nullptr, infos);
    PointsCodecReader reader_b(second.get(), 2, 1, live, infos);
    std::vector<CodecReader *> readers{&reader_a, &reader_b};

    SegmentInfo merged_info = segment("_2", 0);
    auto merge_state = MergeState::create(readers, merged_info, std::make_shared<InfoStreamEnum>());
    ASSERT_TRUE(merge_state.has_value()) << merge_state.error().to_string();
    merge_state.value()->merge_field_infos = infos;
    ASSERT_EQ(merge_state.value()->segment_info.max_doc, 4);

    SegmentInfo write_info = segment("_2", merge_state.value()->segment_info.max_doc);
    SegmentWriteState write_state(directory, infos, io_context_default());
    auto writer = Lucene90PointsFormat::fields_writer(write_state, write_info);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    auto merged = writer.value()->merge(*merge_state.value());
    ASSERT_TRUE(merged.has_value()) << merged.error().to_string();
    ASSERT_TRUE(writer.value()->close().has_value());

    SegmentInfo read_info = segment("_2", 4);
    SegmentReadState read_state(directory, infos, io_context_default());
    auto reader = Lucene90PointsFormat::fields_reader(read_state, read_info);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    ASSERT_TRUE(reader.value()->check_integrity().has_value());

    auto values = reader.value()->get_values("pt");
    ASSERT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
    ASSERT_NE(values.value(), nullptr);
    EXPECT_EQ(values.value()->size().value(), 4U);
    EXPECT_EQ(values.value()->get_doc_count().value(), 4);

    auto tree = values.value()->get_point_tree();
    ASSERT_TRUE(tree.has_value());
    CollectVisitor collector;
    ASSERT_TRUE(tree.value()->visit_doc_values(collector).has_value());
    std::ranges::sort(collector.pairs);
    // Only doc 0 of the second segment is live; doc 1 (value 50) is deleted.
    EXPECT_EQ(collector.pairs, (std::vector<std::pair<int32_t, int32_t>>{{0, 10}, {1, 20}, {2, 30}, {3, 40}}));
}

TEST(PointsMergeTest, FieldWithoutPointsInAnySegmentIsSkipped) {
    ByteBuffersDirectory directory;

    auto first = write_segment(directory, "_0", {7});
    ASSERT_TRUE(first != nullptr);
    auto infos = std::make_shared<const FieldInfos>(FieldInfos::of({point_field(0, "pt")}).value());

    PointsCodecReader reader(first.get(), 1, 1, nullptr, infos);
    std::vector<CodecReader *> readers{&reader};

    SegmentInfo merged_info = segment("_1", 0);
    auto merge_state = MergeState::create(readers, merged_info, std::make_shared<InfoStreamEnum>());
    ASSERT_TRUE(merge_state.has_value());
    // A segment whose reader slot is missing contributes nothing.
    merge_state.value()->points_readers[0] = nullptr;
    merge_state.value()->merge_field_infos = infos;

    SegmentInfo write_info = segment("_1", 0);
    SegmentWriteState write_state(directory, infos, io_context_default());
    auto writer = Lucene90PointsFormat::fields_writer(write_state, write_info);
    ASSERT_TRUE(writer.has_value());

    auto merged = writer.value()->merge(*merge_state.value());
    ASSERT_TRUE(merged.has_value()) << merged.error().to_string();
    ASSERT_TRUE(writer.value()->close().has_value());

    SegmentInfo read_info = segment("_1", 0);
    SegmentReadState read_state(directory, infos, io_context_default());
    auto reader_out = Lucene90PointsFormat::fields_reader(read_state, read_info);
    ASSERT_TRUE(reader_out.has_value()) << (reader_out ? "" : reader_out.error().to_string());
    // The field does not make it into the merged segment's metadata.
    auto values = reader_out.value()->get_values("pt");
    ASSERT_TRUE(values.has_value());
    EXPECT_EQ(values.value(), nullptr);
}
