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

module lucene_core:bkd_offline_point.impl;

import :bkd_offline_point;

import :bkd_config;
import :bkd_heap_point_writer;
import :bit_util;
import :codec_util;
import :data_input;
import :data_output;
import :debug;
import :directory;
import :error;
import :index_input;
import :index_output;
import :io_context;
import :types;

import std;

namespace lucene {

// ---- OfflinePointReader ----

OfflinePointReader::OfflinePointReader(const BKDConfig &config, std::unique_ptr<IndexInput> input, size_t length,
                                       std::vector<uint8_t> reusable_buffer)
    : count_left_(length), input_(std::move(input)), config_(config),
      point_value_(config, std::move(reusable_buffer)) {
    max_points_on_heap_ = point_value_.value().size() / config_.bytes_per_doc();
    debug_check(max_points_on_heap_ > 0, "the reusable buffer must hold at least one point");
}

Result<OfflinePointReader> OfflinePointReader::open(const BKDConfig &config, const Directory &dir,
                                                    std::string_view temp_file_name, size_t start, size_t length,
                                                    std::vector<uint8_t> reusable_buffer) {
    const size_t bytes_per_doc = config.bytes_per_doc();
    auto file_length = dir.file_length(temp_file_name);
    if (!file_length.has_value()) {
        return err(file_length.error());
    }
    if ((start + length) * bytes_per_doc + CodecUtil::footer_length() > file_length.value()) {
        return err(LuceneError::illegal_argument(
            "requested slice is beyond the length of this file: start=" + std::to_string(start) + " length=" +
            std::to_string(length) + " bytesPerDoc=" + std::to_string(bytes_per_doc) + " fileLength=" +
            std::to_string(file_length.value()) + " tempFileName=" + std::string(temp_file_name)));
    }
    if (reusable_buffer.size() < bytes_per_doc) {
        return err(LuceneError::illegal_argument("the reusable buffer must be bigger than " +
                                                 std::to_string(bytes_per_doc) + " bytes"));
    }

    auto input = dir.open_input(temp_file_name, io_context_read_once());
    if (!input.has_value()) {
        return err(input.error());
    }
    if (auto seek = input.value()->seek(start * bytes_per_doc); !seek.has_value()) {
        return err(seek.error());
    }
    return OfflinePointReader(config, std::move(input.value()), length, std::move(reusable_buffer));
}

Result<bool> OfflinePointReader::next() {
    const size_t bytes_per_doc = config_.bytes_per_doc();
    // A reference: this buffer is the point cursor's storage, so copying it
    // would leave every point reading zeroes.
    auto &buffer = point_value_.value();

    if (points_in_buffer_ == 0) {
        if (count_left_ == 0) {
            return false;
        }
        size_t read_len = 0;
        if (count_left_ > max_points_on_heap_) {
            read_len = max_points_on_heap_ * bytes_per_doc;
            points_in_buffer_ = max_points_on_heap_ - 1;
            count_left_ -= max_points_on_heap_;
        } else {
            read_len = count_left_ * bytes_per_doc;
            points_in_buffer_ = count_left_ - 1;
            count_left_ = 0;
        }
        if (auto read = input_->read_bytes(std::span<uint8_t>(buffer.data(), read_len)); !read.has_value()) {
            return err(read.error());
        }
        offset_ = 0;
    } else {
        --points_in_buffer_;
        offset_ += bytes_per_doc;
    }
    return true;
}

HeapPointValue &OfflinePointReader::point_value() {
    debug_check(!closed_, "the reader is closed");
    point_value_.set_offset(offset_);
    return point_value_;
}

VoidResult OfflinePointReader::close() {
    if (closed_) {
        return {};
    }
    closed_ = true;
    if (input_ != nullptr) {
        auto closed = input_->close();
        input_.reset();
        if (!closed.has_value()) {
            return closed;
        }
    }
    return {};
}

std::string OfflinePointReader::to_string() const {
    return "OfflinePointReader(remaining=" + std::to_string(remaining()) + ")";
}

// ---- OfflinePointWriter ----

OfflinePointWriter::OfflinePointWriter(std::unique_ptr<IndexOutput> out, std::string name, const BKDConfig &config,
                                       size_t expected_count)
    : out_(std::move(out)), name_(std::move(name)), config_(config), expected_count_(expected_count) {}

OfflinePointWriter::~OfflinePointWriter() {
    if (!closed_ && out_ != nullptr) {
        (void)close();
    }
}

OfflinePointWriter::OfflinePointWriter(OfflinePointWriter &&) noexcept = default;
OfflinePointWriter &OfflinePointWriter::operator=(OfflinePointWriter &&) noexcept = default;

Result<OfflinePointWriter> OfflinePointWriter::create(const BKDConfig &config, Directory &dir,
                                                      std::string_view temp_file_name_prefix, std::string_view desc,
                                                      size_t expected_count) {
    const std::string suffix = "bkd_" + std::string(desc);
    auto created = dir.create_temp_output(temp_file_name_prefix, suffix, io_context_default());
    if (!created.has_value()) {
        return err(created.error());
    }
    std::string name = created->name;
    auto out = std::move(created->output);
    return OfflinePointWriter(std::move(out), std::move(name), config, expected_count);
}

VoidResult OfflinePointWriter::append_bytes(std::span<const uint8_t> packed_value, int32_t doc_id) {
    debug_check(!closed_, "point writer is already closed");
    debug_check(packed_value.size() == config_.packed_bytes_length(),
                "the packed value must be packed_bytes_length() long");
    debug_check(expected_count_ == 0 || count_ < expected_count_, "more points than expected");

    if (out_ == nullptr) {
        return err(LuceneError::illegal_state("point writer is already closed"));
    }
    if (auto written = out_->write_bytes(packed_value); !written.has_value()) {
        return written;
    }
    // `write_int` is little endian here (like the rest of this port), while the
    // BKD record stores its doc id big endian, matching the reference implementation's
    // `write_int(i32::to_be(docId))`.
    std::array<uint8_t, BitUtil::INT_BYTES> doc_bytes{};
    BitUtil::set_i32_be(doc_bytes, 0, doc_id);
    if (auto written = out_->write_bytes(doc_bytes); !written.has_value()) {
        return written;
    }
    ++count_;
    return {};
}

VoidResult OfflinePointWriter::append_packed(std::span<const uint8_t> record) {
    debug_check(!closed_, "point writer is already closed");
    debug_check(record.size() == config_.bytes_per_doc(), "a packed record is bytes_per_doc() long");
    debug_check(expected_count_ == 0 || count_ < expected_count_, "more points than expected");

    if (out_ == nullptr) {
        return err(LuceneError::illegal_state("point writer is already closed"));
    }
    if (auto written = out_->write_bytes(record); !written.has_value()) {
        return written;
    }
    ++count_;
    return {};
}

VoidResult OfflinePointWriter::close() {
    if (closed_) {
        return {};
    }
    closed_ = true;
    if (out_ == nullptr) {
        return {};
    }
    auto footer = CodecUtil::write_footer(*out_);
    auto closed_output = out_->close();
    out_.reset();
    if (!footer.has_value()) {
        return footer;
    }
    return closed_output;
}

Result<OfflinePointReader> OfflinePointWriter::make_reader(const Directory &dir, size_t start, size_t length,
                                                           std::vector<uint8_t> reusable_buffer) const {
    if (!closed_) {
        return err(LuceneError::illegal_state("the point writer must be closed before reading"));
    }
    if (start + length > count_) {
        // The slice comes from the BKD writer's own arithmetic, so a mismatch is
        // a corrupted request rather than a caller bug worth aborting on.
        return err(LuceneError::illegal_argument("the requested slice exceeds the " + std::to_string(count_) +
                                                 " written points (start=" + std::to_string(start) + " length=" +
                                                 std::to_string(length) + ")"));
    }
    debug_check(expected_count_ == 0 || count_ == expected_count_, "expected count mismatch");
    return OfflinePointReader::open(config_, dir, name_, start, length, std::move(reusable_buffer));
}

VoidResult OfflinePointWriter::destroy(Directory &dir) { return dir.delete_file(name_); }

std::string OfflinePointWriter::to_string() const {
    return "OfflinePointWriter(count=" + std::to_string(count_) + " tempFileName=" + name_ + ")";
}

} // namespace lucene
