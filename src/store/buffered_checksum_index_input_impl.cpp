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

module lucene_core:buffered_checksum_index_input.impl;

import :buffered_checksum;
import :buffered_checksum_index_input;
import :check_sum_index_input;
import :checksum;
import :debug;
import :error;
import :index_input;
import :types;

import std;

namespace lucene {

BufferedChecksumIndexInput::BufferedChecksumIndexInput(std::unique_ptr<IndexInput> main)
    : main_(std::move(main)), digest_(std::make_unique<Crc32Checksum>()) {
    debug_check(main_ != nullptr);
}

Result<size_t> BufferedChecksumIndexInput::get_file_pointer() const {
    return main_->get_file_pointer();
}

Result<size_t> BufferedChecksumIndexInput::length() const {
    return main_->length();
}

Result<uint8_t> BufferedChecksumIndexInput::read_byte() {
    auto value = main_->read_byte();
    if (!value) {
        return std::unexpected(std::move(value.error()));
    }
    digest_.update(value.value());
    return value;
}

VoidResult BufferedChecksumIndexInput::read_bytes(std::span<uint8_t> b) {
    if (auto read = main_->read_bytes(b); !read) {
        return read;
    }
    digest_.update_bytes(b, 0, b.size());
    return {};
}

VoidResult BufferedChecksumIndexInput::skip_bytes(int64_t num_bytes) {
    // Skips are not checksummed, matching the reference implementation (which delegates to
    // `IndexInput::skip_bytes`); use `skip_by_reading` when the checksum of the
    // skipped bytes matters.
    return IndexInput::skip_bytes(num_bytes);
}

Result<int16_t> BufferedChecksumIndexInput::read_short() {
    auto value = main_->read_short();
    if (!value) {
        return std::unexpected(std::move(value.error()));
    }
    digest_.update_short(value.value());
    return value;
}

Result<int32_t> BufferedChecksumIndexInput::read_int() {
    auto value = main_->read_int();
    if (!value) {
        return std::unexpected(std::move(value.error()));
    }
    digest_.update_int(value.value());
    return value;
}

Result<int64_t> BufferedChecksumIndexInput::read_long() {
    auto value = main_->read_long();
    if (!value) {
        return std::unexpected(std::move(value.error()));
    }
    digest_.update_long(value.value());
    return value;
}

Result<std::unique_ptr<IndexInput>> BufferedChecksumIndexInput::clone() const {
    return err(LuceneError::unsupported_operation("BufferedChecksumIndexInput cannot be cloned"));
}

Result<std::unique_ptr<RandomAccessInput>> BufferedChecksumIndexInput::random_access_slice(size_t /*offset*/,
                                                                                         size_t /*length*/) const {
    return err(LuceneError::unsupported_operation(
            "BufferedChecksumIndexInput does not support random access slicing"));
}

VoidResult BufferedChecksumIndexInput::prefetch(size_t pos, size_t len) {
    return main_->prefetch(pos, len);
}

VoidResult BufferedChecksumIndexInput::close() {
    return main_->close();
}

int64_t BufferedChecksumIndexInput::get_checksum() {
    return digest_.get_value();
}

VoidResult BufferedChecksumIndexInput::skip_by_reading(size_t num_bytes) {
    return CheckSumIndexInput::skip_by_reading(num_bytes);
}

std::string BufferedChecksumIndexInput::to_string() const {
    return std::format("BufferedChecksumIndexInput({})", main_->to_string());
}

} // namespace lucene
