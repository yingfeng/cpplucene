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

module lucene_core:lucene90_points_format.impl;

import :lucene90_points_format;

import :error;
import :lucene90_points_reader;
import :lucene90_points_writer;
import :points_format;
import :points_reader;
import :points_writer;
import :segment_info;
import :segment_read_state;
import :segment_write_state;
import :types;

import std;

namespace lucene {

Result<std::unique_ptr<PointsWriter>> Lucene90PointsFormat::fields_writer(SegmentWriteState &state,
                                                                         const SegmentInfo &segment_info) {
    auto writer = Lucene90PointsWriter::create_with_default_config(state, segment_info);
    if (!writer.has_value()) {
        return std::unexpected(std::move(writer.error()));
    }
    return std::unique_ptr<PointsWriter>(std::move(writer.value()));
}

Result<std::unique_ptr<PointsReader>> Lucene90PointsFormat::fields_reader(SegmentReadState &state,
                                                                         const SegmentInfo &segment_info) {
    auto reader = Lucene90PointsReader::create(state, segment_info);
    if (!reader.has_value()) {
        return std::unexpected(std::move(reader.error()));
    }
    return std::unique_ptr<PointsReader>(std::move(reader.value()));
}

} // namespace lucene

namespace lucene {

Result<std::unique_ptr<PointsWriter>> Lucene90PointsFormatAdapter::fields_writer(
        SegmentWriteState &state, const SegmentInfo &segment_info) const {
    return Lucene90PointsFormat::fields_writer(state, segment_info);
}

Result<std::unique_ptr<PointsReader>> Lucene90PointsFormatAdapter::fields_reader(
        SegmentReadState &state, const SegmentInfo &segment_info) const {
    return Lucene90PointsFormat::fields_reader(state, segment_info);
}

} // namespace lucene
