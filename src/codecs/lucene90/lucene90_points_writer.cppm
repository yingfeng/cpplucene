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

export module lucene_core:lucene90_points_writer;

import :types;
import :error;

import std;

import :bkd_config;
import :directory;
import :field_infos;
import :index_output;
import :io_context;
import :lucene90_points_format;
import :points_reader;
import :points_writer;
import :segment_info;
import :segment_write_state;

namespace lucene {

/// Writes the `.kdd`/`.kdi`/`.kdm` triple of one segment.
///
/// Port of `Lucene90PointsWriter`
/// (`codecs/lucene90/lucene90_points_writer.rs`): all three files are opened and
/// header-stamped up front, every field is appended by [`write_field`], and
/// [`finish`] writes the terminating `-1` field number, the footers and the two
/// file pointers the reader needs.
///
/// The tree itself is built by [`BkdWriter`] from this port's `util/bkd`
/// package.  Fields whose tree ends up empty (no point at all) write nothing,
/// which is what rlucene does by returning `None` from its BKD `finish`.
export class Lucene90PointsWriter final : public PointsWriter {
public:
    /// Creates the writer with Lucene's defaults: 512 points per leaf and a
    /// 16 MB sort budget in heap.
    [[nodiscard]] static Result<std::unique_ptr<Lucene90PointsWriter>> create_with_default_config(
            SegmentWriteState &state, const SegmentInfo &segment_info);

    /// Creates the writer with an explicit leaf size and heap budget.
    ///
    /// `max_mb_sort_in_heap` is converted to a point count the same way rlucene
    /// does (`max_mb * 1 MiB / bytes_per_doc`), so the BKD writer knows when to
    /// spill to a temporary file.
    [[nodiscard]] static Result<std::unique_ptr<Lucene90PointsWriter>> create(SegmentWriteState &state,
                                                                             const SegmentInfo &segment_info,
                                                                             size_t max_points_in_leaf_node,
                                                                             double max_mb_sort_in_heap);

    ~Lucene90PointsWriter() override;

    Lucene90PointsWriter(const Lucene90PointsWriter &) = delete;
    Lucene90PointsWriter &operator=(const Lucene90PointsWriter &) = delete;

    [[nodiscard]] VoidResult write_field(const FieldInfo &field_info, PointsReader &reader) override;

    [[nodiscard]] VoidResult finish() override;

    [[nodiscard]] VoidResult close() override;

private:
    Lucene90PointsWriter(SegmentWriteState &state, const SegmentInfo &segment_info, size_t max_points_in_leaf_node,
                         double max_mb_sort_in_heap);

    /// Opens the three outputs and stamps their index headers.
    [[nodiscard]] VoidResult open_outputs();

    /// `max_mb_sort_in_heap` as a point count, for [`BkdWriter::create`].
    [[nodiscard]] size_t max_points_in_heap(const BKDConfig &config) const;

    Directory &directory_;
    std::string segment_name_;
    std::string segment_suffix_;
    /// Copy of the segment id, which every header of this segment carries.
    std::vector<uint8_t> segment_id_;
    const IOContext &context_;
    int32_t max_doc_{0};

    std::unique_ptr<IndexOutput> data_out_;
    std::unique_ptr<IndexOutput> meta_out_;
    std::unique_ptr<IndexOutput> index_out_;

    size_t max_points_in_leaf_node_;
    double max_mb_sort_in_heap_;
    bool finished_{false};
};

} // namespace lucene
