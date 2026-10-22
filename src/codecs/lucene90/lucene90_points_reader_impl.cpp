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

module lucene_core:lucene90_points_reader.impl;

import :lucene90_points_reader;

import :bkd_index_tree;
import :bkd_reader;
import :check_sum_index_input;
import :codec_util;
import :data_input;
import :directory;
import :error;
import :field_infos;
import :index_input;
import :io_context;
import :lucene90_points_format;
import :point_values;
import :points_reader;
import :segment_info;
import :segment_read_state;
import :types;

import std;

namespace lucene {

Lucene90PointsReader::Lucene90PointsReader(std::unique_ptr<IndexInput> index_in, std::unique_ptr<IndexInput> data_in,
                                           std::shared_ptr<const FieldInfos> field_infos)
    : index_in_(std::move(index_in)), data_in_(std::move(data_in)), field_infos_(std::move(field_infos)) {}

Lucene90PointsReader::~Lucene90PointsReader() {
    static_cast<void>(close());
}

Result<std::unique_ptr<Lucene90PointsReader>> Lucene90PointsReader::create(SegmentReadState &state,
                                                                           const SegmentInfo &segment_info) {
    auto index_context = state.context.with_read_advice_self(ReadAdvice::RandomPreload);
    if (!index_context.has_value()) {
        return std::unexpected(std::move(index_context.error()));
    }
    // Points read whole ranges of bytes at once, so the data file is opened with
    // `Normal` advice to let the directory prefetch ahead.
    auto data_context = state.context.with_read_advice_self(ReadAdvice::Normal);
    if (!data_context.has_value()) {
        return std::unexpected(std::move(data_context.error()));
    }

    const std::span<const uint8_t> id(segment_info.id);
    const std::string& suffix = state.segment_suffix;

    const std::string index_file =
            segment_file_name(segment_info.name, suffix, Lucene90PointsFormat::INDEX_EXTENSION);
    auto index_in = state.directory.open_input(index_file, index_context.value());
    if (!index_in.has_value()) {
        return std::unexpected(std::move(index_in.error()));
    }
    std::unique_ptr<IndexInput> index_input = std::move(index_in.value());
    if (auto header = CodecUtil::check_index_header(*index_input, Lucene90PointsFormat::INDEX_CODEC_NAME,
                                                    Lucene90PointsFormat::VERSION_START,
                                                    Lucene90PointsFormat::VERSION_CURRENT, id, suffix);
        !header.has_value()) {
        return std::unexpected(std::move(header.error()));
    }
    if (auto checksum = CodecUtil::retrieve_checksum(*index_input); !checksum.has_value()) {
        return std::unexpected(std::move(checksum.error()));
    }

    const std::string data_file = segment_file_name(segment_info.name, suffix, Lucene90PointsFormat::DATA_EXTENSION);
    auto data_in = state.directory.open_input(data_file, data_context.value());
    if (!data_in.has_value()) {
        return std::unexpected(std::move(data_in.error()));
    }
    std::unique_ptr<IndexInput> data_input = std::move(data_in.value());
    if (auto header = CodecUtil::check_index_header(*data_input, Lucene90PointsFormat::DATA_CODEC_NAME,
                                                    Lucene90PointsFormat::VERSION_START,
                                                    Lucene90PointsFormat::VERSION_CURRENT, id, suffix);
        !header.has_value()) {
        return std::unexpected(std::move(header.error()));
    }
    if (auto checksum = CodecUtil::retrieve_checksum(*data_input); !checksum.has_value()) {
        return std::unexpected(std::move(checksum.error()));
    }

    std::unique_ptr<Lucene90PointsReader> reader(
            new Lucene90PointsReader(std::move(index_input), std::move(data_input), state.field_infos));
    if (auto read = reader->read_meta(state, segment_info); !read.has_value()) {
        return std::unexpected(std::move(read.error()));
    }
    return reader;
}

VoidResult Lucene90PointsReader::read_meta(SegmentReadState &state, const SegmentInfo &segment_info) {
    const std::span<const uint8_t> id(segment_info.id);
    const std::string meta_file = segment_file_name(segment_info.name, state.segment_suffix,
                                                    Lucene90PointsFormat::META_EXTENSION);
    auto meta_in = state.directory.open_checksum_input(meta_file);
    if (!meta_in.has_value()) {
        return std::unexpected(std::move(meta_in.error()));
    }

    if (auto header = CodecUtil::check_index_header(*meta_in.value(), Lucene90PointsFormat::META_CODEC_NAME,
                                                    Lucene90PointsFormat::VERSION_START,
                                                    Lucene90PointsFormat::VERSION_CURRENT, id, state.segment_suffix);
        !header.has_value()) {
        return std::unexpected(std::move(header.error()));
    }

    while (true) {
        auto field_number = meta_in.value()->read_int();
        if (!field_number.has_value()) {
            return std::unexpected(std::move(field_number.error()));
        }
        if (field_number.value() == -1) {
            break;
        }
        if (field_number.value() < 0) {
            return err(LuceneError::corrupt_index("illegal point field number: " +
                                                  std::to_string(field_number.value())));
        }

        auto meta = BkdMeta::read(*meta_in.value());
        if (!meta.has_value()) {
            return std::unexpected(std::move(meta.error()));
        }
        auto tree = BkdIndexTree::read(*index_in_, meta.value());
        if (!tree.has_value()) {
            return std::unexpected(std::move(tree.error()));
        }

        auto field_data = data_in_->clone();
        if (!field_data.has_value()) {
            return std::unexpected(std::move(field_data.error()));
        }
        auto bkd = BkdReader::open(std::make_shared<const BkdMeta>(std::move(meta.value())),
                                   std::make_shared<const BkdIndexTree>(std::move(tree.value())),
                                   std::move(field_data.value()));
        if (!bkd.has_value()) {
            return std::unexpected(std::move(bkd.error()));
        }
        readers_.insert_or_assign(field_number.value(), std::move(bkd.value()));
    }

    // The metadata ends with the final lengths of the two data files, which have to
    // match what was opened — a truncated or stale file is caught here rather than
    // in the middle of a query.
    auto index_length = meta_in.value()->read_long();
    if (!index_length.has_value()) {
        return std::unexpected(std::move(index_length.error()));
    }
    auto data_length = meta_in.value()->read_long();
    if (!data_length.has_value()) {
        return std::unexpected(std::move(data_length.error()));
    }

    // Structural check only; the CRCs are validated by `check_integrity`, which is
    // how the rest of this port treats checksum inputs.
    if (auto footer = CodecUtil::check_footer(*meta_in.value()); !footer.has_value()) {
        return std::unexpected(std::move(footer.error()));
    }

    if (index_length.value() < 0 || data_length.value() < 0) {
        return err(LuceneError::corrupt_index("the points metadata has a negative file length"));
    }
    if (auto checksum =
                CodecUtil::retrieve_checksum_with_expected(*index_in_, static_cast<size_t>(index_length.value()));
        !checksum.has_value()) {
        return std::unexpected(std::move(checksum.error()));
    }
    if (auto checksum =
                CodecUtil::retrieve_checksum_with_expected(*data_in_, static_cast<size_t>(data_length.value()));
        !checksum.has_value()) {
        return std::unexpected(std::move(checksum.error()));
    }
    return {};
}

VoidResult Lucene90PointsReader::check_integrity() {
    if (auto checked = CodecUtil::checksum_entire_file(*index_in_); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    if (auto checked = CodecUtil::checksum_entire_file(*data_in_); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    return {};
}

Result<std::shared_ptr<PointValues>> Lucene90PointsReader::get_values(std::string_view field_name) const {
    const FieldInfo *field_info = field_infos_ == nullptr ? nullptr : field_infos_->by_name(field_name);
    if (field_info == nullptr) {
        return err(LuceneError::illegal_state("field \"" + std::string(field_name) + "\" is unrecognized"));
    }
    if (field_info->point_dimension_count == 0) {
        return err(LuceneError::illegal_state("field \"" + std::string(field_name) + "\" does not index point values"));
    }
    auto found = readers_.find(field_info->number);
    if (found == readers_.end()) {
        return std::shared_ptr<PointValues>(nullptr);
    }
    return std::shared_ptr<PointValues>(found->second);
}

VoidResult Lucene90PointsReader::close() {
    readers_.clear();

    VoidResult result{};
    if (data_in_ != nullptr) {
        if (auto closed = data_in_->close(); !closed.has_value()) {
            result = closed;
        }
        data_in_.reset();
    }
    if (index_in_ != nullptr) {
        if (auto closed = index_in_->close(); !closed.has_value() && result.has_value()) {
            result = closed;
        }
        index_in_.reset();
    }
    return result;
}

} // namespace lucene
