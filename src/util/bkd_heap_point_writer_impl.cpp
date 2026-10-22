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

module lucene_core:bkd_heap_point_writer.impl;

import :bkd_heap_point_writer;

import :bkd_config;
import :bkd_util;
import :bit_util;
import :core_helper;
import :debug;
import :error;
import :types;

import std;

namespace lucene {

// ---- HeapPointValue ----

HeapPointValue::HeapPointValue(const BKDConfig &config, std::vector<uint8_t> value)
    : value_(std::move(value)), packed_value_length_(config.packed_bytes_length()),
      packed_value_doc_id_length_(config.bytes_per_doc()) {}

std::span<const uint8_t> HeapPointValue::packed_value() const {
    debug_check(offset_ + packed_value_length_ <= value_.size(), "the packed value must fit into the block");
    return std::span<const uint8_t>(value_.data() + offset_, packed_value_length_);
}

int32_t HeapPointValue::doc_id() const {
    const size_t position = offset_ + packed_value_length_;
    debug_check(position + BitUtil::INT_BYTES <= value_.size(), "the doc id must fit into the block");
    return BitUtil::get_i32_be(value_, position);
}

std::span<const uint8_t> HeapPointValue::packed_value_doc_id_bytes() const {
    debug_check(offset_ + packed_value_doc_id_length_ <= value_.size(), "the record must fit into the block");
    return std::span<const uint8_t>(value_.data() + offset_, packed_value_doc_id_length_);
}

// ---- HeapPointReader ----

HeapPointReader::HeapPointReader(HeapPointValue points, size_t start, size_t end, size_t bytes_per_doc)
    : points_(std::move(points)), cur_read_(start), end_(end), bytes_per_doc_(bytes_per_doc), started_(false) {}

bool HeapPointReader::next() {
    if (!started_) {
        started_ = true;
        // `cur_read_` already sits on `start`.
        return cur_read_ < end_;
    }
    ++cur_read_;
    return cur_read_ < end_;
}

void HeapPointReader::require_position() const {
    debug_check(started_, "the reader was not advanced yet");
    debug_check(cur_read_ < end_, "the reader is exhausted");
}

std::span<const uint8_t> HeapPointReader::packed_value() {
    require_position();
    points_.set_offset(bytes_per_doc_ * cur_read_);
    return points_.packed_value();
}

int32_t HeapPointReader::doc_id() {
    require_position();
    points_.set_offset(bytes_per_doc_ * cur_read_);
    return points_.doc_id();
}

// ---- HeapPointWriter ----

HeapPointWriter::HeapPointWriter(const BKDConfig &config, size_t size)
    : block_(config.bytes_per_doc() * size, 0), size_(size), config_(config),
      scratch_(config.bytes_per_doc(), 0),
      data_dims_and_doc_length_(config.bytes_per_doc() - config.packed_index_bytes_length()), next_write_(0),
      closed_(false) {
    if (size > 0) {
        point_value_.emplace(config_, std::vector<uint8_t>{});
    }
}

void HeapPointWriter::require_closed() const {
    debug_check(closed_, "the point writer must be closed first");
}

std::span<const uint8_t> HeapPointWriter::point_bytes() const {
    debug_check(point_value_.has_value(), "the point value was handed to a reader");
    return point_value_->value();
}

VoidResult HeapPointWriter::append_bytes(std::span<const uint8_t> packed_value, int32_t doc_id) {
    debug_check(!closed_, "point writer is already closed");
    debug_check(packed_value.size() == config_.packed_bytes_length(),
                "the packed value must be packed_bytes_length() long");
    debug_check(next_write_ < size_, "the writer is full");

    const size_t position = next_write_ * config_.bytes_per_doc();
    std::copy(packed_value.begin(), packed_value.end(), block_.begin() + static_cast<std::ptrdiff_t>(position));
    BitUtil::set_i32_be(block_, position + config_.packed_bytes_length(), doc_id);
    ++next_write_;
    return {};
}

VoidResult HeapPointWriter::append_packed(std::span<const uint8_t> packed_value_with_doc_id) {
    debug_check(!closed_, "point writer is already closed");
    debug_check(next_write_ < size_, "the writer is full");
    debug_check(packed_value_with_doc_id.size() == config_.bytes_per_doc(),
                "a packed record is bytes_per_doc() long");

    const size_t position = next_write_ * config_.bytes_per_doc();
    std::copy(packed_value_with_doc_id.begin(), packed_value_with_doc_id.end(),
              block_.begin() + static_cast<std::ptrdiff_t>(position));
    ++next_write_;
    return {};
}

VoidResult HeapPointWriter::close() {
    closed_ = true;
    if (point_value_.has_value()) {
        // `block_` is frozen now, so its ownership moves into the cursor.
        point_value_->value() = std::move(block_);
    }
    return {};
}

int32_t HeapPointWriter::byte_at(size_t i, size_t k) const {
    require_closed();
    const auto bytes = point_bytes();
    debug_check(i * config_.bytes_per_doc() + k < bytes.size(), "the point index is out of range");
    return static_cast<int32_t>(bytes[i * config_.bytes_per_doc() + k]);
}

VoidResult HeapPointWriter::copy_dim(size_t i, size_t dim, std::span<uint8_t> bytes, size_t offset) const {
    require_closed();
    const auto block = point_bytes();
    const size_t start = i * config_.bytes_per_doc() + dim;
    debug_check(start + config_.bytes_per_dim <= block.size(), "the dimension must fit into the block");
    debug_check(offset + config_.bytes_per_dim <= bytes.size(), "the destination must have room");
    std::copy_n(block.begin() + static_cast<std::ptrdiff_t>(start), config_.bytes_per_dim,
                bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    return {};
}

VoidResult HeapPointWriter::copy_data_dims_and_doc(size_t i, std::span<uint8_t> bytes, size_t offset) const {
    require_closed();
    const auto block = point_bytes();
    const size_t start = i * config_.bytes_per_doc() + config_.packed_index_bytes_length();
    debug_check(start + data_dims_and_doc_length_ <= block.size(), "the record must fit into the block");
    debug_check(offset + data_dims_and_doc_length_ <= bytes.size(), "the destination must have room");
    std::copy_n(block.begin() + static_cast<std::ptrdiff_t>(start), data_dims_and_doc_length_,
                bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    return {};
}

int32_t HeapPointWriter::compare_dim(size_t i, size_t j, size_t dim) const {
    require_closed();
    const auto block = point_bytes();
    const auto bytes_per_doc = config_.bytes_per_doc();
    return BKDUtil::compare_unsigned(block, i * bytes_per_doc + dim, block, j * bytes_per_doc + dim,
                                     config_.bytes_per_dim);
}

int32_t HeapPointWriter::compare_dim_with_scratch(size_t j, std::span<const uint8_t> dim_value, size_t offset,
                                                  size_t dim) const {
    require_closed();
    const auto block = point_bytes();
    return BKDUtil::compare_unsigned(dim_value, offset, block, j * config_.bytes_per_doc() + dim,
                                     config_.bytes_per_dim);
}

int32_t HeapPointWriter::compare_data_dims_and_doc(size_t i, size_t j) const {
    require_closed();
    const auto block = point_bytes();
    const size_t base = config_.packed_index_bytes_length();
    const auto bytes_per_doc = config_.bytes_per_doc();
    const auto i_offset = i * bytes_per_doc + base;
    const auto j_offset = j * bytes_per_doc + base;
    return BKDUtil::compare_unsigned(block, i_offset, block, j_offset, data_dims_and_doc_length_);
}

int32_t HeapPointWriter::compare_data_dims_and_doc_with(size_t j, std::span<const uint8_t> data_dims_and_docs,
                                                        size_t offset) const {
    require_closed();
    const auto block = point_bytes();
    const auto j_offset = j * config_.bytes_per_doc() + config_.packed_index_bytes_length();
    return BKDUtil::compare_unsigned(data_dims_and_docs, offset, block, j_offset, data_dims_and_doc_length_);
}

size_t HeapPointWriter::compute_cardinality(size_t from, size_t to,
                                            std::span<const size_t> common_prefix_lengths) const {
    require_closed();
    const auto block = point_bytes();
    const auto bytes_per_doc = config_.bytes_per_doc();
    const auto bytes_per_dim = config_.bytes_per_dim;

    size_t leaf_cardinality = 1;
    for (size_t i = from + 1; i < to; ++i) {
        const size_t point_offset = (i - 1) * bytes_per_doc;
        const size_t next_point_offset = point_offset + bytes_per_doc;
        for (size_t dim = 0; dim < config_.num_dims && dim < common_prefix_lengths.size(); ++dim) {
            const size_t start = dim * bytes_per_dim + common_prefix_lengths[dim];
            const size_t end = dim * bytes_per_dim + bytes_per_dim;
            const int32_t mismatch = CoreHelper::mismatch_u8(
                block.subspan(next_point_offset + start, end - start), block.subspan(point_offset + start, end - start));
            if (mismatch != -1) {
                ++leaf_cardinality;
                break;
            }
        }
    }
    return leaf_cardinality;
}

HeapPointValue &HeapPointWriter::packed_value(size_t index) {
    require_closed();
    debug_check(point_value_.has_value(), "the point value was handed to a reader");
    debug_check(index < next_write_, "the point index is out of range");
    point_value_->set_offset(index * config_.bytes_per_doc());
    return point_value_.value();
}

void HeapPointWriter::swap(size_t i, size_t j) {
    require_closed();
    auto &value = point_value_->value();
    const size_t bytes_per_doc = config_.bytes_per_doc();
    const size_t index_i = bytes_per_doc * i;
    const size_t index_j = bytes_per_doc * j;
    std::copy_n(value.begin() + static_cast<std::ptrdiff_t>(index_i), bytes_per_doc, scratch_.begin());
    std::copy_n(value.begin() + static_cast<std::ptrdiff_t>(index_j), bytes_per_doc,
                value.begin() + static_cast<std::ptrdiff_t>(index_i));
    std::copy_n(scratch_.begin(), bytes_per_doc, value.begin() + static_cast<std::ptrdiff_t>(index_j));
}

Result<HeapPointReader> HeapPointWriter::make_reader(size_t start, size_t length) {
    require_closed();
    debug_check(start + length <= size_, "the requested slice must fit into the writer");
    debug_check(start + length <= next_write_, "the requested slice must contain written points");
    debug_check(point_value_.has_value(), "the point value was already handed to a reader");

    const size_t end = start + length;
    HeapPointValue value = std::move(point_value_.value());
    point_value_.reset();
    return HeapPointReader(std::move(value), start, end, config_.bytes_per_doc());
}

std::string HeapPointWriter::to_string() const {
    return "HeapPointWriter(count=" + std::to_string(next_write_) + " size=" + std::to_string(size_) + ")";
}

} // namespace lucene
