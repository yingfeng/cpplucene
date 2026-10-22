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

module lucene_core:lucene90_points_writer.impl;

import :lucene90_points_writer;

import :bkd_config;
import :bkd_writer;
import :codec_util;
import :directory;
import :error;
import :field_infos;
import :index_output;
import :io_context;
import :lucene90_points_format;
import :point_values;
import :points_reader;
import :points_writer;
import :segment_info;
import :segment_write_state;
import :types;

import std;

namespace lucene {

namespace {

/// The reference implementation's `DEFAULT_MAX_MB_SORT_IN_HEAP`.
constexpr double DEFAULT_MAX_MB_SORT_IN_HEAP = 16.0;

/// Feeds every point of a reader into a [`BkdWriter`].
///
/// The visitor never reports "cell outside" or "cell inside", so a point tree
/// always hands over the individual points (the reference implementation's `IntersectVisitorImpl` in
/// `lucene90_points_writer.rs` does the same).
class AddPointsVisitor final : public IntersectVisitor {
public:
    explicit AddPointsVisitor(BkdWriter &writer) : writer_(writer) {}

    [[nodiscard]] VoidResult visit(int32_t /*doc_id*/) override {
        return err(LuceneError::illegal_state("visit(doc_id) must not be called when copying point values"));
    }

    [[nodiscard]] VoidResult visit_with_packed_value(int32_t doc_id, std::span<const uint8_t> packed_value) override {
        return writer_.add(packed_value, doc_id);
    }

    [[nodiscard]] Result<Relation> compare(std::span<const uint8_t> /*min_packed_value*/,
                                           std::span<const uint8_t> /*max_packed_value*/) const override {
        return Relation::kCellCrossesQuery;
    }

private:
    BkdWriter &writer_;
};

} // namespace

Lucene90PointsWriter::Lucene90PointsWriter(SegmentWriteState &state, const SegmentInfo &segment_info,
                                           size_t max_points_in_leaf_node, double max_mb_sort_in_heap)
    : directory_(state.directory),
      segment_name_(segment_info.name),
      segment_suffix_(state.segment_suffix),
      segment_id_(segment_info.id),
      context_(state.context),
      max_doc_(segment_info.max_doc),
      max_points_in_leaf_node_(max_points_in_leaf_node),
      max_mb_sort_in_heap_(max_mb_sort_in_heap) {}

Lucene90PointsWriter::~Lucene90PointsWriter() {
    // Best effort: a writer that never reached `finish` still has to release the
    // file handles, and a failing close cannot be reported from a destructor.
    static_cast<void>(close());
}

Result<std::unique_ptr<Lucene90PointsWriter>> Lucene90PointsWriter::create_with_default_config(
        SegmentWriteState &state, const SegmentInfo &segment_info) {
    return create(state, segment_info, BKDConfig::DEFAULT_MAX_POINTS_IN_LEAF_NODE, DEFAULT_MAX_MB_SORT_IN_HEAP);
}

Result<std::unique_ptr<Lucene90PointsWriter>> Lucene90PointsWriter::create(SegmentWriteState &state,
                                                                           const SegmentInfo &segment_info,
                                                                           size_t max_points_in_leaf_node,
                                                                           double max_mb_sort_in_heap) {
    std::unique_ptr<Lucene90PointsWriter> writer(new Lucene90PointsWriter(
            state, segment_info, max_points_in_leaf_node, max_mb_sort_in_heap));
    if (auto opened = writer->open_outputs(); !opened.has_value()) {
        return std::unexpected(std::move(opened.error()));
    }
    return writer;
}

VoidResult Lucene90PointsWriter::open_outputs() {
    const std::span<const uint8_t> id(segment_id_);

    const std::string data_file =
            segment_file_name(segment_name_, segment_suffix_, Lucene90PointsFormat::DATA_EXTENSION);
    auto data_out = directory_.create_output(data_file, context_);
    if (!data_out.has_value()) {
        return std::unexpected(std::move(data_out.error()));
    }
    data_out_ = std::move(data_out.value());
    if (auto written = CodecUtil::write_index_header(*data_out_, Lucene90PointsFormat::DATA_CODEC_NAME,
                                                     Lucene90PointsFormat::VERSION_CURRENT, id, segment_suffix_);
        !written.has_value()) {
        return written;
    }

    const std::string meta_file =
            segment_file_name(segment_name_, segment_suffix_, Lucene90PointsFormat::META_EXTENSION);
    auto meta_out = directory_.create_output(meta_file, context_);
    if (!meta_out.has_value()) {
        return std::unexpected(std::move(meta_out.error()));
    }
    meta_out_ = std::move(meta_out.value());
    if (auto written = CodecUtil::write_index_header(*meta_out_, Lucene90PointsFormat::META_CODEC_NAME,
                                                     Lucene90PointsFormat::VERSION_CURRENT, id, segment_suffix_);
        !written.has_value()) {
        return written;
    }

    const std::string index_file =
            segment_file_name(segment_name_, segment_suffix_, Lucene90PointsFormat::INDEX_EXTENSION);
    auto index_out = directory_.create_output(index_file, context_);
    if (!index_out.has_value()) {
        return std::unexpected(std::move(index_out.error()));
    }
    index_out_ = std::move(index_out.value());
    return CodecUtil::write_index_header(*index_out_, Lucene90PointsFormat::INDEX_CODEC_NAME,
                                          Lucene90PointsFormat::VERSION_CURRENT, id, segment_suffix_);
}

size_t Lucene90PointsWriter::max_points_in_heap(const BKDConfig &config) const {
    if (max_mb_sort_in_heap_ <= 0) {
        return 0;
    }
    const size_t bytes_per_doc = config.bytes_per_doc();
    if (bytes_per_doc == 0) {
        return 0;
    }
    return static_cast<size_t>(max_mb_sort_in_heap_ * 1024.0 * 1024.0 / static_cast<double>(bytes_per_doc));
}

VoidResult Lucene90PointsWriter::write_field(const FieldInfo &field_info, PointsReader &reader) {
    auto values = reader.get_values(field_info.name);
    if (!values.has_value()) {
        return std::unexpected(std::move(values.error()));
    }
    const std::shared_ptr<PointValues> &points = values.value();
    if (points == nullptr) {
        return err(LuceneError::illegal_state("the reader holds no point values for field " + field_info.name));
    }

    auto point_tree = points->get_point_tree();
    if (!point_tree.has_value()) {
        return std::unexpected(std::move(point_tree.error()));
    }

    auto config = BKDConfig::create(static_cast<size_t>(field_info.point_dimension_count),
                                    static_cast<size_t>(field_info.point_index_dimension_count),
                                    static_cast<size_t>(field_info.point_num_bytes), max_points_in_leaf_node_);
    if (!config.has_value()) {
        return std::unexpected(std::move(config.error()));
    }
    const BKDConfig &bkd_config = config.value();

    auto total_point_count = points->size();
    if (!total_point_count.has_value()) {
        return std::unexpected(std::move(total_point_count.error()));
    }
    if (total_point_count.value() == 0) {
        // A field that contributed no point gets no entry in the metadata, so the
        // reader reports no values for it.  The reference implementation reaches the same state through
        // its BKD writer's empty `finish`; the BKD writer of this port rejects an
        // empty tree up front, so the skip happens here instead.
        return {};
    }

    auto builder = BkdWriter::create(bkd_config, total_point_count.value(), max_doc_, &directory_,
                                     max_points_in_heap(bkd_config));
    if (!builder.has_value()) {
        return std::unexpected(std::move(builder.error()));
    }
    BkdWriter &writer = builder.value();

    AddPointsVisitor visitor(writer);
    if (auto visited = point_tree.value()->visit_doc_values(visitor); !visited.has_value()) {
        static_cast<void>(writer.close());
        return visited;
    }

    auto data = writer.finish(*data_out_);
    if (!data.has_value()) {
        static_cast<void>(writer.close());
        return std::unexpected(std::move(data.error()));
    }

    VoidResult written{};
    if (data.value().point_count > 0) {
        // The field number goes in front of the tree metadata; a reader loops over
        // these pairs until it sees the terminating `-1`.
        if (auto field_number = meta_out_->write_int(field_info.number); !field_number.has_value()) {
            written = field_number;
        } else {
            written = writer.write_index(*meta_out_, index_out_.get(), data.value());
        }
    }

    auto closed = writer.close();
    if (!written.has_value()) {
        return written;
    }
    return closed;
}

VoidResult Lucene90PointsWriter::finish() {
    if (finished_) {
        return err(LuceneError::illegal_state("the points writer is already finished"));
    }
    finished_ = true;

    if (auto written = meta_out_->write_int(-1); !written.has_value()) {
        return written;
    }
    if (auto written = CodecUtil::write_footer(*index_out_); !written.has_value()) {
        return written;
    }
    if (auto written = CodecUtil::write_footer(*data_out_); !written.has_value()) {
        return written;
    }

    // The two lengths are read back by the reader to validate the files; they have
    // to include the footers written just above.
    auto index_length = index_out_->get_file_pointer();
    if (!index_length.has_value()) {
        return std::unexpected(std::move(index_length.error()));
    }
    if (auto written = meta_out_->write_long(static_cast<int64_t>(index_length.value())); !written.has_value()) {
        return written;
    }

    auto data_length = data_out_->get_file_pointer();
    if (!data_length.has_value()) {
        return std::unexpected(std::move(data_length.error()));
    }
    if (auto written = meta_out_->write_long(static_cast<int64_t>(data_length.value())); !written.has_value()) {
        return written;
    }

    return CodecUtil::write_footer(*meta_out_);
}

VoidResult Lucene90PointsWriter::close() {
    VoidResult result{};
    if (meta_out_ != nullptr) {
        if (auto closed = meta_out_->close(); !closed.has_value()) {
            result = closed;
        }
        meta_out_.reset();
    }
    if (index_out_ != nullptr) {
        if (auto closed = index_out_->close(); !closed.has_value() && result.has_value()) {
            result = closed;
        }
        index_out_.reset();
    }
    if (data_out_ != nullptr) {
        if (auto closed = data_out_->close(); !closed.has_value() && result.has_value()) {
            result = closed;
        }
        data_out_.reset();
    }
    return result;
}

} // namespace lucene
