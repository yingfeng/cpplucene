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

module lucene_core:point_values_writer.impl;

import :point_values_writer;

import std;

import :bytes_ref;
import :error;
import :field_infos;
import :point_values;
import :points_reader;
import :points_writer;
import :types;

namespace lucene {

namespace {

/// The buffered points of one field: `doc_ids[i]` owns the packed value that
/// starts at `bytes[i * packed_length]`.
struct BufferedPoints {
    std::vector<int32_t> doc_ids;
    std::vector<uint8_t> bytes;
    size_t packed_length{0};
    size_t num_dimensions{0};
    size_t num_index_dimensions{0};
    size_t bytes_per_dimension{0};
    int32_t num_docs{0};

    [[nodiscard]] std::span<const uint8_t> value_at(size_t index) const {
        return std::span<const uint8_t>(bytes.data() + index * packed_length, packed_length);
    }
};

/// The packed value made of the per-dimension extremes of every buffered point.
[[nodiscard]] BytesRef extreme_packed_value(const BufferedPoints &points, bool minimum) {
    const size_t bytes_per_dim = points.bytes_per_dimension;
    std::vector<uint8_t> packed(points.packed_length);
    for (size_t dim = 0; dim < points.num_dimensions; ++dim) {
        const size_t offset = dim * bytes_per_dim;
        std::span<uint8_t> out(packed.data() + offset, bytes_per_dim);
        if (points.doc_ids.empty()) {
            continue;
        }
        const std::span<const uint8_t> first = points.value_at(0);
        std::copy(first.begin() + static_cast<std::ptrdiff_t>(offset),
                  first.begin() + static_cast<std::ptrdiff_t>(offset + bytes_per_dim), out.begin());
        for (size_t index = 1; index < points.doc_ids.size(); ++index) {
            const std::span<const uint8_t> value = points.value_at(index);
            for (size_t i = 0; i < bytes_per_dim; ++i) {
                const uint8_t byte = value[offset + i];
                if (minimum ? byte < out[i] : byte > out[i]) {
                    out[i] = byte;
                }
            }
        }
    }
    return BytesRef(std::move(packed));
}

/// A leaf-only [`PointTree`] over the buffered points.
class BufferedPointTree final : public PointTree {
public:
    explicit BufferedPointTree(std::shared_ptr<const BufferedPoints> points) : points_(std::move(points)) {}

    [[nodiscard]] Result<BytesRef> get_min_packed_value() override { return extreme_packed_value(*points_, true); }
    [[nodiscard]] Result<BytesRef> get_max_packed_value() override { return extreme_packed_value(*points_, false); }

    [[nodiscard]] Result<size_t> size() override { return points_->doc_ids.size(); }

    [[nodiscard]] VoidResult visit_doc_ids(IntersectVisitor &visitor) override {
        for (const int32_t doc_id : points_->doc_ids) {
            if (auto visited = visitor.visit(doc_id); !visited.has_value()) {
                return visited;
            }
        }
        return {};
    }

    [[nodiscard]] VoidResult visit_doc_values(IntersectVisitor &visitor) override {
        for (size_t index = 0; index < points_->doc_ids.size(); ++index) {
            if (auto visited = visitor.visit_with_packed_value(points_->doc_ids[index], points_->value_at(index));
                !visited.has_value()) {
                return visited;
            }
        }
        return {};
    }

    [[nodiscard]] std::unique_ptr<PointTree> try_clone() const override {
        return std::unique_ptr<PointTree>(new BufferedPointTree(points_));
    }

private:
    std::shared_ptr<const BufferedPoints> points_;
};

/// The [`PointValues`] of a buffered field, as the points writer reads it.
class BufferedPointValues final : public PointValues {
public:
    explicit BufferedPointValues(std::shared_ptr<const BufferedPoints> points) : points_(std::move(points)) {}

    [[nodiscard]] Result<std::optional<BytesRef>> get_min_packed_value() const override {
        if (points_->doc_ids.empty()) {
            return std::optional<BytesRef>{};
        }
        return std::optional<BytesRef>(extreme_packed_value(*points_, true));
    }

    [[nodiscard]] Result<std::optional<BytesRef>> get_max_packed_value() const override {
        if (points_->doc_ids.empty()) {
            return std::optional<BytesRef>{};
        }
        return std::optional<BytesRef>(extreme_packed_value(*points_, false));
    }

    [[nodiscard]] Result<size_t> get_num_dimensions() const override { return points_->num_dimensions; }
    [[nodiscard]] Result<size_t> get_num_index_dimensions() const override { return points_->num_index_dimensions; }
    [[nodiscard]] Result<size_t> get_bytes_per_dimension() const override { return points_->bytes_per_dimension; }
    [[nodiscard]] Result<size_t> size() const override { return points_->doc_ids.size(); }
    [[nodiscard]] Result<int32_t> get_doc_count() const override { return points_->num_docs; }

    [[nodiscard]] Result<std::unique_ptr<PointTree>> get_point_tree() const override {
        return std::unique_ptr<PointTree>(new BufferedPointTree(points_));
    }

private:
    std::shared_ptr<const BufferedPoints> points_;
};

/// The single-field [`PointsReader`] the points writer copies from.
class BufferedPointsReader final : public PointsReader {
public:
    BufferedPointsReader(std::string field, std::shared_ptr<PointValues> values)
        : field_(std::move(field)), values_(std::move(values)) {}

    [[nodiscard]] VoidResult check_integrity() override { return {}; }

    [[nodiscard]] Result<std::shared_ptr<PointValues>> get_values(std::string_view field_name) const override {
        if (field_name != field_) {
            return err(LuceneError::illegal_argument("fieldName must be the same"));
        }
        return values_;
    }

    [[nodiscard]] VoidResult close() override { return {}; }

private:
    std::string field_;
    std::shared_ptr<PointValues> values_;
};

} // namespace

PointValuesWriter::PointValuesWriter(const FieldInfo &field_info)
        : field_info_(&field_info),
          packed_bytes_length_(field_info.point_dimension_count * field_info.point_num_bytes) {}

VoidResult PointValuesWriter::add_packed_value(int32_t doc_id, std::span<const uint8_t> value) {
    if (value.size() != packed_bytes_length_) {
        return err(LuceneError::illegal_argument(
                "field=" + field_info_->name + ": this field's value has length=" + std::to_string(value.size()) +
                " but should be " + std::to_string(packed_bytes_length_)));
    }
    bytes_.insert(bytes_.end(), value.begin(), value.end());
    doc_ids_.push_back(doc_id);
    if (doc_id != last_doc_id_) {
        ++num_docs_;
        last_doc_id_ = doc_id;
    }
    ++num_points_;
    return {};
}

VoidResult PointValuesWriter::flush(PointsWriter &writer) {
    auto points = std::make_shared<BufferedPoints>();
    points->doc_ids = std::move(doc_ids_);
    points->bytes = std::move(bytes_);
    points->packed_length = packed_bytes_length_;
    points->num_dimensions = field_info_->point_dimension_count;
    points->num_index_dimensions = field_info_->point_index_dimension_count;
    points->bytes_per_dimension = field_info_->point_num_bytes;
    points->num_docs = num_docs_;
    doc_ids_.clear();
    bytes_.clear();

    std::shared_ptr<PointValues> values = std::make_shared<BufferedPointValues>(std::move(points));
    BufferedPointsReader reader(field_info_->name, std::move(values));
    return writer.write_field(*field_info_, reader);
}

} // namespace lucene
