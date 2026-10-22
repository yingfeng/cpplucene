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
#include <functional>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.lucene90_points;

import lucene_core;

using namespace lucene;

namespace {

constexpr size_t BYTES_PER_DIM = 4;

// ---- a test-only point source -------------------------------------------------
//
// `Lucene90PointsWriter::write_field` copies the points out of a `PointsReader`,
// so writing needs a source tree.  At flush time that source is the mutable point
// tree of the segment buffer; this stand-in is the same shape: one leaf, no
// children, every point visited in insertion order.

class InMemoryPointValues;

class InMemoryPointTree final : public PointTree {
public:
    explicit InMemoryPointTree(const InMemoryPointValues &values) : values_(values) {}

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
    const InMemoryPointValues &values_;
};

class InMemoryPointValues final : public PointValues {
public:
    InMemoryPointValues(size_t num_dims, size_t num_index_dims, size_t bytes_per_dim,
                        std::vector<std::vector<uint8_t>> values, std::vector<int32_t> doc_ids)
        : num_dims_(num_dims),
          num_index_dims_(num_index_dims),
          bytes_per_dim_(bytes_per_dim),
          values_(std::move(values)),
          doc_ids_(std::move(doc_ids)) {
        compute_bounds();
    }

    [[nodiscard]] Result<std::optional<BytesRef>> get_min_packed_value() const override {
        return std::optional<BytesRef>(BytesRef(min_value_));
    }
    [[nodiscard]] Result<std::optional<BytesRef>> get_max_packed_value() const override {
        return std::optional<BytesRef>(BytesRef(max_value_));
    }
    [[nodiscard]] Result<size_t> get_num_dimensions() const override { return num_dims_; }
    [[nodiscard]] Result<size_t> get_num_index_dimensions() const override { return num_index_dims_; }
    [[nodiscard]] Result<size_t> get_bytes_per_dimension() const override { return bytes_per_dim_; }
    [[nodiscard]] Result<size_t> size() const override { return values_.size(); }
    [[nodiscard]] Result<int32_t> get_doc_count() const override {
        std::set<int32_t> docs(doc_ids_.begin(), doc_ids_.end());
        return static_cast<int32_t>(docs.size());
    }
    [[nodiscard]] Result<std::unique_ptr<PointTree>> get_point_tree() const override {
        return std::unique_ptr<PointTree>(new InMemoryPointTree(*this));
    }

    [[nodiscard]] const std::vector<std::vector<uint8_t>> &values() const noexcept { return values_; }
    [[nodiscard]] const std::vector<int32_t> &doc_ids() const noexcept { return doc_ids_; }
    [[nodiscard]] const std::vector<uint8_t> &min_value() const noexcept { return min_value_; }
    [[nodiscard]] const std::vector<uint8_t> &max_value() const noexcept { return max_value_; }

private:
    void compute_bounds() {
        const size_t index_bytes = num_index_dims_ * bytes_per_dim_;
        if (values_.empty()) {
            min_value_.assign(index_bytes, 0);
            max_value_.assign(index_bytes, 0);
            return;
        }
        min_value_.assign(values_.front().begin(),
                          values_.front().begin() + static_cast<std::ptrdiff_t>(index_bytes));
        max_value_ = min_value_;
        for (const auto &value : values_) {
            for (size_t i = 0; i < index_bytes; ++i) {
                min_value_[i] = std::min(min_value_[i], value[i]);
                max_value_[i] = std::max(max_value_[i], value[i]);
            }
        }
    }

    size_t num_dims_;
    size_t num_index_dims_;
    size_t bytes_per_dim_;
    std::vector<std::vector<uint8_t>> values_;
    std::vector<int32_t> doc_ids_;
    std::vector<uint8_t> min_value_;
    std::vector<uint8_t> max_value_;
};

Result<BytesRef> InMemoryPointTree::get_min_packed_value() {
    return BytesRef(values_.min_value());
}

Result<BytesRef> InMemoryPointTree::get_max_packed_value() {
    return BytesRef(values_.max_value());
}

Result<size_t> InMemoryPointTree::size() {
    return values_.values().size();
}

VoidResult InMemoryPointTree::visit_doc_ids(IntersectVisitor &visitor) {
    for (const int32_t doc_id : values_.doc_ids()) {
        if (auto visited = visitor.visit(doc_id); !visited.has_value()) {
            return visited;
        }
    }
    return {};
}

VoidResult InMemoryPointTree::visit_doc_values(IntersectVisitor &visitor) {
    const auto &points = values_.values();
    const auto &docs = values_.doc_ids();
    for (size_t i = 0; i < points.size(); ++i) {
        if (auto visited = visitor.visit_with_packed_value(docs[i], std::span<const uint8_t>(points[i]));
            !visited.has_value()) {
            return visited;
        }
    }
    return {};
}

std::unique_ptr<PointTree> InMemoryPointTree::try_clone() const {
    return std::unique_ptr<PointTree>(new InMemoryPointTree(values_));
}

class InMemoryPointsReader final : public PointsReader {
public:
    InMemoryPointsReader(std::string field_name, std::shared_ptr<PointValues> values)
        : field_name_(std::move(field_name)), values_(std::move(values)) {}

    [[nodiscard]] VoidResult check_integrity() override { return {}; }

    [[nodiscard]] VoidResult close() override { return {}; }

    [[nodiscard]] Result<std::shared_ptr<PointValues>> get_values(std::string_view field_name) const override {
        if (field_name != field_name_) {
            return err(LuceneError::illegal_state("field \"" + std::string(field_name) + "\" is unrecognized"));
        }
        return values_;
    }

private:
    std::string field_name_;
    std::shared_ptr<PointValues> values_;
};

/// Accepts the documents whose points fall inside a per-dimension box.
class BoxVisitor final : public IntersectVisitor {
public:
    BoxVisitor(std::vector<int32_t> min, std::vector<int32_t> max, size_t bytes_per_dim)
        : min_(std::move(min)), max_(std::move(max)), bytes_per_dim_(bytes_per_dim) {}

    [[nodiscard]] VoidResult visit(int32_t doc_id) override {
        visited_.push_back(doc_id);
        return {};
    }

    [[nodiscard]] VoidResult visit_with_packed_value(int32_t doc_id, std::span<const uint8_t> packed_value) override {
        for (size_t dim = 0; dim < min_.size(); ++dim) {
            const int32_t value = BitUtil::get_i32_be(packed_value, dim * bytes_per_dim_);
            if (value < min_[dim] || value > max_[dim]) {
                return {};
            }
        }
        visited_.push_back(doc_id);
        return {};
    }

    [[nodiscard]] Result<Relation> compare(std::span<const uint8_t> min_packed_value,
                                           std::span<const uint8_t> max_packed_value) const override {
        bool inside = true;
        for (size_t dim = 0; dim < min_.size(); ++dim) {
            const size_t offset = dim * bytes_per_dim_;
            const int32_t cell_min = BitUtil::get_i32_be(min_packed_value, offset);
            const int32_t cell_max = BitUtil::get_i32_be(max_packed_value, offset);
            if (cell_max < min_[dim] || cell_min > max_[dim]) {
                return Relation::kCellOutsideQuery;
            }
            if (cell_min < min_[dim] || cell_max > max_[dim]) {
                inside = false;
            }
        }
        return inside ? Relation::kCellInsideQuery : Relation::kCellCrossesQuery;
    }

    [[nodiscard]] const std::vector<int32_t> &visited() const noexcept { return visited_; }

private:
    std::vector<int32_t> min_;
    std::vector<int32_t> max_;
    size_t bytes_per_dim_;
    std::vector<int32_t> visited_;
};

/// Collects every point the tree reports.
class CollectVisitor final : public IntersectVisitor {
public:
    [[nodiscard]] VoidResult visit(int32_t doc_id) override { return visit_with_packed_value(doc_id, {}); }

    [[nodiscard]] VoidResult visit_with_packed_value(int32_t doc_id, std::span<const uint8_t> packed_value) override {
        docs.push_back(doc_id);
        values.emplace_back(packed_value.begin(), packed_value.end());
        return {};
    }

    [[nodiscard]] Result<Relation> compare(std::span<const uint8_t> /*min_packed_value*/,
                                           std::span<const uint8_t> /*max_packed_value*/) const override {
        return Relation::kCellCrossesQuery;
    }

    [[nodiscard]] std::vector<std::pair<int32_t, std::vector<uint8_t>>> pairs() const {
        std::vector<std::pair<int32_t, std::vector<uint8_t>>> result;
        result.reserve(docs.size());
        for (size_t i = 0; i < docs.size(); ++i) {
            result.emplace_back(docs[i], values[i]);
        }
        std::ranges::sort(result);
        return result;
    }

    std::vector<int32_t> docs;
    std::vector<std::vector<uint8_t>> values;
};

// ---- helpers -----------------------------------------------------------------

SegmentInfo make_segment(int32_t max_doc, std::string name = "_0") {
    SegmentInfo info;
    info.name = std::move(name);
    info.id.assign(ID_LENGTH, 0x5A);
    info.max_doc = max_doc;
    return info;
}

FieldInfo point_field(int32_t number, std::string name, int32_t dims, int32_t index_dims) {
    FieldInfo field;
    field.number = number;
    field.name = std::move(name);
    field.point_dimension_count = dims;
    field.point_index_dimension_count = index_dims;
    field.point_num_bytes = static_cast<int32_t>(BYTES_PER_DIM);
    return field;
}

std::vector<uint8_t> pack(std::initializer_list<int32_t> dims) {
    std::vector<uint8_t> packed(dims.size() * BYTES_PER_DIM, 0);
    size_t i = 0;
    for (const int32_t value : dims) {
        BitUtil::set_i32_be(packed, i * BYTES_PER_DIM, value);
        ++i;
    }
    return packed;
}

} // namespace

TEST(Lucene90PointsTest, RoundTripsASinglePointField) {
    ByteBuffersDirectory directory;
    auto field_infos = std::make_shared<const FieldInfos>(
            FieldInfos::of({point_field(0, "field", 1, 1)}).value());
    SegmentInfo segment_info = make_segment(40);
    SegmentWriteState write_state(directory, field_infos, io_context_default());

    std::vector<std::vector<uint8_t>> points;
    std::vector<int32_t> docs;
    for (int32_t i = 0; i < 40; ++i) {
        points.push_back(pack({i * 3}));
        docs.push_back(i);
    }
    auto source = std::make_shared<InMemoryPointValues>(1, 1, BYTES_PER_DIM, points, docs);
    InMemoryPointsReader source_reader("field", source);

    auto writer = Lucene90PointsFormat::fields_writer(write_state, segment_info);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    const FieldInfo &field = field_infos->fields().front();
    ASSERT_TRUE(writer.value()->write_field(field, source_reader).has_value());
    ASSERT_TRUE(writer.value()->finish().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());

    // All three files exist.
    auto names = directory.list_all();
    ASSERT_TRUE(names.has_value());
    EXPECT_NE(std::find(names->begin(), names->end(), "_0.kdm"), names->end());
    EXPECT_NE(std::find(names->begin(), names->end(), "_0.kdi"), names->end());
    EXPECT_NE(std::find(names->begin(), names->end(), "_0.kdd"), names->end());

    SegmentReadState read_state(directory, field_infos, io_context_default());
    auto reader = Lucene90PointsFormat::fields_reader(read_state, segment_info);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_TRUE(reader.value()->is_lucene90_points_reader());
    ASSERT_TRUE(reader.value()->check_integrity().has_value());

    auto values = reader.value()->get_values("field");
    ASSERT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
    ASSERT_NE(values.value(), nullptr);
    EXPECT_EQ(values.value()->size().value(), 40U);
    EXPECT_EQ(values.value()->get_doc_count().value(), 40);
    EXPECT_EQ(values.value()->get_num_dimensions().value(), 1U);
    EXPECT_EQ(values.value()->get_bytes_per_dimension().value(), BYTES_PER_DIM);
    ASSERT_TRUE(values.value()->get_min_packed_value().has_value());
    EXPECT_EQ(BitUtil::get_i32_be(values.value()->get_min_packed_value().value()->as_slice(), 0), 0);
    ASSERT_TRUE(values.value()->get_max_packed_value().has_value());
    EXPECT_EQ(BitUtil::get_i32_be(values.value()->get_max_packed_value().value()->as_slice(), 0), 117);

    // Every point comes back, and the tree has the four inner nodes a five-leaf
    // tree needs.
    auto point_tree = values.value()->get_point_tree();
    ASSERT_TRUE(point_tree.has_value()) << (point_tree ? "" : point_tree.error().to_string());
    EXPECT_EQ(point_tree.value()->size().value(), 40U);
    CollectVisitor collector;
    ASSERT_TRUE(point_tree.value()->visit_doc_values(collector).has_value());

    std::vector<std::pair<int32_t, std::vector<uint8_t>>> expected;
    for (int32_t i = 0; i < 40; ++i) {
        expected.emplace_back(i, points[static_cast<size_t>(i)]);
    }
    std::ranges::sort(expected);
    EXPECT_EQ(collector.pairs(), expected);

    // A query box only reports the documents inside it.
    BoxVisitor box({10 * 3}, {19 * 3}, BYTES_PER_DIM);
    ASSERT_TRUE(values.value()->intersect(box).has_value());
    std::vector<int32_t> sorted = box.visited();
    std::ranges::sort(sorted);
    std::vector<int32_t> expected_docs;
    for (int32_t i = 10; i <= 19; ++i) {
        expected_docs.push_back(i);
    }
    EXPECT_EQ(sorted, expected_docs);

    EXPECT_TRUE(reader.value()->close().has_value());
}

TEST(Lucene90PointsTest, RoundTripsSeveralLeavesAndWalksTheTree) {
    ByteBuffersDirectory directory;
    auto field_infos = std::make_shared<const FieldInfos>(
            FieldInfos::of({point_field(0, "field", 1, 1)}).value());
    SegmentInfo segment_info = make_segment(100);
    SegmentWriteState write_state(directory, field_infos, io_context_default());

    // 100 points with 8 per leaf: 13 leaves, so the tree is unbalanced and the
    // last leaf holds the remainder.
    std::vector<std::vector<uint8_t>> points;
    std::vector<int32_t> docs;
    for (int32_t i = 0; i < 100; ++i) {
        points.push_back(pack({i}));
        docs.push_back(i);
    }
    auto source = std::make_shared<InMemoryPointValues>(1, 1, BYTES_PER_DIM, points, docs);
    InMemoryPointsReader source_reader("field", source);

    auto writer = Lucene90PointsWriter::create(write_state, segment_info, /*max_points_in_leaf_node=*/8,
                                               /*max_mb_sort_in_heap=*/16.0);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    const FieldInfo &field = field_infos->fields().front();
    ASSERT_TRUE(writer.value()->write_field(field, source_reader).has_value());
    ASSERT_TRUE(writer.value()->finish().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());

    SegmentReadState read_state(directory, field_infos, io_context_default());
    auto reader = Lucene90PointsFormat::fields_reader(read_state, segment_info);
    ASSERT_TRUE(reader.has_value());
    auto values = reader.value()->get_values("field");
    ASSERT_TRUE(values.has_value());

    auto point_tree = values.value()->get_point_tree();
    ASSERT_TRUE(point_tree.has_value());
    EXPECT_EQ(point_tree.value()->size().value(), 100U);

    // Depth-first walk with one cursor per node: every inner node has a left and a
    // right child, every leaf holds points, and the subtree sizes shrink on the
    // way down.
    size_t leaves = 0;
    size_t inner_nodes = 0;
    std::function<void(PointTree &)> walk = [&](PointTree &cursor) {
        const size_t size_here = cursor.size().value();
        if (!cursor.move_to_child().value()) {
            ++leaves;
            EXPECT_GT(size_here, 0U);
            return;
        }
        ++inner_nodes;
        // `cursor` now sits on the left child.  Restore the parent, then clone a
        // cursor per child so the walk never disturbs the parent.
        ASSERT_TRUE(cursor.move_to_parent().value());

        auto left = cursor.try_clone();
        ASSERT_NE(left, nullptr);
        ASSERT_TRUE(left->move_to_child().value());
        auto right = cursor.try_clone();
        ASSERT_NE(right, nullptr);
        ASSERT_TRUE(right->move_to_child().value());
        ASSERT_TRUE(right->move_to_sibling().value());

        // The children partition the parent's points.
        EXPECT_EQ(left->size().value() + right->size().value(), size_here);
        // And their bounds lie inside the parent's.
        const size_t left_min = static_cast<size_t>(BitUtil::get_i32_be(left->get_min_packed_value().value().as_slice(), 0));
        const size_t right_max = static_cast<size_t>(BitUtil::get_i32_be(right->get_max_packed_value().value().as_slice(), 0));
        EXPECT_LT(left_min, right_max);

        walk(*left);
        walk(*right);

        // The cursor itself never moved.
        EXPECT_EQ(cursor.size().value(), size_here);
    };
    walk(*point_tree.value());
    EXPECT_EQ(leaves, 13U);
    EXPECT_EQ(inner_nodes, 12U);
}

TEST(Lucene90PointsTest, RoundTripsTwoDimensionsAndTwoFields) {
    ByteBuffersDirectory directory;
    auto field_infos = std::make_shared<const FieldInfos>(
            FieldInfos::of({point_field(0, "latlon", 2, 2), point_field(1, "height", 1, 1)}).value());
    SegmentInfo segment_info = make_segment(12);
    SegmentWriteState write_state(directory, field_infos, io_context_default());

    std::vector<std::vector<uint8_t>> latlon;
    std::vector<int32_t> doc_ids;
    for (int32_t i = 0; i < 12; ++i) {
        latlon.push_back(pack({i, i * 10}));
        doc_ids.push_back(i);
    }
    auto latlon_values = std::make_shared<InMemoryPointValues>(2, 2, BYTES_PER_DIM, latlon, doc_ids);
    InMemoryPointsReader latlon_reader("latlon", latlon_values);

    std::vector<std::vector<uint8_t>> height;
    std::vector<int32_t> height_docs;
    for (int32_t i = 0; i < 5; ++i) {
        height.push_back(pack({1000 + i}));
        height_docs.push_back(i * 2);
    }
    auto height_values = std::make_shared<InMemoryPointValues>(1, 1, BYTES_PER_DIM, height, height_docs);
    InMemoryPointsReader height_reader("height", height_values);

    // A third field without points must be rejected by the reader.
    auto all_fields = std::make_shared<const FieldInfos>(FieldInfos::of({
            point_field(0, "latlon", 2, 2),
            point_field(1, "height", 1, 1),
            FieldInfo{.number = 2, .name = "text", .attributes = {}},
    }).value());
    SegmentWriteState three_field_state(directory, all_fields, io_context_default());

    auto writer = Lucene90PointsFormat::fields_writer(three_field_state, segment_info);
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer.value()->write_field(all_fields->fields()[0], latlon_reader).has_value());
    ASSERT_TRUE(writer.value()->write_field(all_fields->fields()[1], height_reader).has_value());
    // The text field has no point values at all: the reader's own check rejects it.
    EXPECT_FALSE(writer.value()->write_field(all_fields->fields()[2], latlon_reader).has_value());
    ASSERT_TRUE(writer.value()->finish().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());

    SegmentReadState read_state(directory, all_fields, io_context_default());
    auto reader = Lucene90PointsFormat::fields_reader(read_state, segment_info);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());

    auto latlon_point_values = reader.value()->get_values("latlon");
    ASSERT_TRUE(latlon_point_values.has_value());
    EXPECT_EQ(latlon_point_values.value()->size().value(), 12U);
    EXPECT_EQ(latlon_point_values.value()->get_num_index_dimensions().value(), 2U);

    auto height_point_values = reader.value()->get_values("height");
    ASSERT_TRUE(height_point_values.has_value());
    EXPECT_EQ(height_point_values.value()->size().value(), 5U);

    // Both dimensions of the box are honoured.
    BoxVisitor box({3, 30}, {6, 60}, BYTES_PER_DIM);
    ASSERT_TRUE(latlon_point_values.value()->intersect(box).has_value());
    std::vector<int32_t> sorted = box.visited();
    std::ranges::sort(sorted);
    EXPECT_EQ(sorted, (std::vector<int32_t>{3, 4, 5, 6}));

    // A box that only overlaps in the first dimension still crosses the cells and
    // filters on the second one.
    BoxVisitor partial({3, 100}, {6, 200}, BYTES_PER_DIM);
    ASSERT_TRUE(latlon_point_values.value()->intersect(partial).has_value());
    EXPECT_TRUE(partial.visited().empty());

    // The non-point field is reported by name and by dimension count.
    EXPECT_FALSE(reader.value()->get_values("missing").has_value());
    EXPECT_FALSE(reader.value()->get_values("text").has_value());
}

TEST(Lucene90PointsTest, HandlesASingleLeafAndNoPoints) {
    ByteBuffersDirectory directory;
    auto field_infos = std::make_shared<const FieldInfos>(
            FieldInfos::of({point_field(0, "one", 1, 1), point_field(1, "empty", 1, 1)}).value());
    SegmentInfo segment_info = make_segment(1);
    SegmentWriteState write_state(directory, field_infos, io_context_default());

    auto one = std::make_shared<InMemoryPointValues>(1, 1, BYTES_PER_DIM, std::vector<std::vector<uint8_t>>{pack({7})},
                                                    std::vector<int32_t>{0});
    InMemoryPointsReader one_reader("one", one);
    auto empty = std::make_shared<InMemoryPointValues>(1, 1, BYTES_PER_DIM, std::vector<std::vector<uint8_t>>{},
                                                      std::vector<int32_t>{});
    InMemoryPointsReader empty_reader("empty", empty);

    auto writer = Lucene90PointsFormat::fields_writer(write_state, segment_info);
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer.value()->write_field(field_infos->fields()[0], one_reader).has_value());
    // A field with no points contributes nothing to the metadata.
    ASSERT_TRUE(writer.value()->write_field(field_infos->fields()[1], empty_reader).has_value());
    ASSERT_TRUE(writer.value()->finish().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());

    SegmentReadState read_state(directory, field_infos, io_context_default());
    auto reader = Lucene90PointsFormat::fields_reader(read_state, segment_info);
    ASSERT_TRUE(reader.has_value());

    auto values = reader.value()->get_values("one");
    ASSERT_TRUE(values.has_value());
    EXPECT_EQ(values.value()->size().value(), 1U);
    auto point_tree = values.value()->get_point_tree();
    ASSERT_TRUE(point_tree.has_value());
    // The root of a single-leaf tree has no children.
    EXPECT_FALSE(point_tree.value()->move_to_child().value());
    EXPECT_EQ(point_tree.value()->size().value(), 1U);
    CollectVisitor collector;
    ASSERT_TRUE(point_tree.value()->visit_doc_ids(collector).has_value());
    EXPECT_EQ(collector.docs, std::vector<int32_t>{0});

    // The empty field never reached the metadata, so it has no reader.
    auto empty_values = reader.value()->get_values("empty");
    ASSERT_TRUE(empty_values.has_value());
    EXPECT_EQ(empty_values.value(), nullptr);
}

TEST(Lucene90PointsTest, ValidatesHeadersAgainstTheSegment) {
    ByteBuffersDirectory directory;
    auto field_infos = std::make_shared<const FieldInfos>(
            FieldInfos::of({point_field(0, "field", 1, 1)}).value());
    SegmentInfo segment_info = make_segment(4);
    SegmentWriteState write_state(directory, field_infos, io_context_default());

    auto points = std::make_shared<InMemoryPointValues>(1, 1, BYTES_PER_DIM, std::vector<std::vector<uint8_t>>{pack({1})},
                                                       std::vector<int32_t>{0});
    InMemoryPointsReader source("field", points);

    auto writer = Lucene90PointsFormat::fields_writer(write_state, segment_info);
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer.value()->write_field(field_infos->fields()[0], source).has_value());
    ASSERT_TRUE(writer.value()->finish().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());

    // A different segment id means the files belong to another segment.
    SegmentInfo other = make_segment(4);
    other.id.assign(ID_LENGTH, 0x11);
    SegmentReadState read_state(directory, field_infos, io_context_default());
    EXPECT_FALSE(Lucene90PointsFormat::fields_reader(read_state, other).has_value());
}
