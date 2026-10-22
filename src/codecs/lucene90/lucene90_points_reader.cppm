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

export module lucene_core:lucene90_points_reader;

import :types;
import :error;

import std;

import :bkd_reader;
import :field_infos;
import :index_input;
import :lucene90_points_format;
import :point_values;
import :points_reader;
import :segment_info;
import :segment_read_state;

namespace lucene {

/// Reads the `.kdd`/`.kdi`/`.kdm` triple of one segment.
///
/// Port of `Lucene90PointsReader`
/// (`codecs/lucene90/lucene90_points_reader.rs`): it opens the two big files up
/// front (validating their headers and footers), then walks the `.kdm` metadata
/// once, building one [`BkdReader`] per point field.  The index file is opened
/// with `RandomPreload` advice and the data file with `Normal`, because the leaf
/// blocks are read as contiguous ranges.
///
/// The readers are keyed by field number and handed out by name; the metadata
/// also stores the final lengths of the index and data files, which the reader
/// validates before trusting them.
export class Lucene90PointsReader final : public PointsReader {
public:
    /// Opens the reader for `state`.
    [[nodiscard]] static Result<std::unique_ptr<Lucene90PointsReader>> create(SegmentReadState &state,
                                                                             const SegmentInfo &segment_info);

    ~Lucene90PointsReader() override;

    Lucene90PointsReader(const Lucene90PointsReader &) = delete;
    Lucene90PointsReader &operator=(const Lucene90PointsReader &) = delete;

    [[nodiscard]] VoidResult check_integrity() override;

    [[nodiscard]] Result<std::shared_ptr<PointValues>> get_values(std::string_view field_name) const override;

    [[nodiscard]] bool is_lucene90_points_reader() const override { return true; }

    /// Closes both inputs and drops the per-field readers.
    [[nodiscard]] VoidResult close() override;

private:
    Lucene90PointsReader(std::unique_ptr<IndexInput> index_in, std::unique_ptr<IndexInput> data_in,
                         std::shared_ptr<const FieldInfos> field_infos);

    /// The reference implementation reads through the metadata file once, collecting readers; an error
    /// half way leaves this reader closing its inputs, so the work is done here.
    [[nodiscard]] VoidResult read_meta(SegmentReadState &state, const SegmentInfo &segment_info);

    std::unique_ptr<IndexInput> index_in_;
    std::unique_ptr<IndexInput> data_in_;
    std::map<int32_t, std::shared_ptr<BkdReader>> readers_;
    std::shared_ptr<const FieldInfos> field_infos_;
};

} // namespace lucene
