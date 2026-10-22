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

module lucene_core:buffered_checksum.impl;

import :buffered_checksum;
import :checksum;
import :debug;
import :types;

import std;

namespace lucene {

namespace {

/// `value.to_le_bytes()` of rlucene.
template <class T>
std::array<uint8_t, sizeof(T)> to_le_bytes(T value) {
    using Unsigned = std::make_unsigned_t<T>;
    const auto bits = static_cast<Unsigned>(value);
    std::array<uint8_t, sizeof(T)> bytes{};
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<uint8_t>((bits >> (8 * i)) & 0xFFU);
    }
    return bytes;
}

} // namespace

BufferedChecksum::BufferedChecksum(std::unique_ptr<Checksum> checksum)
    : BufferedChecksum(std::move(checksum), DEFAULT_BUFFER_SIZE) {}

BufferedChecksum::BufferedChecksum(std::unique_ptr<Checksum> checksum, size_t buffer_size)
    : buffer_(buffer_size, 0), checksum_(std::move(checksum)) {
    debug_check(checksum_ != nullptr);
}

void BufferedChecksum::flush() {
    if (upto_ > 0) {
        checksum_->update_bytes(buffer_, 0, upto_);
        upto_ = 0;
    }
}

void BufferedChecksum::update(uint8_t b) {
    if (upto_ == buffer_.size()) {
        flush();
    }
    buffer_[upto_] = b;
    ++upto_;
}

void BufferedChecksum::update_bytes(std::span<const uint8_t> bytes, size_t offset, size_t len) {
    debug_check(offset + len <= bytes.size());
    const size_t available = buffer_.size() - upto_;
    if (len <= available) {
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), len,
                    buffer_.begin() + static_cast<std::ptrdiff_t>(upto_));
        upto_ += len;
        return;
    }
    flush();
    if (len > buffer_.size()) {
        checksum_->update_bytes(bytes, offset, len);
    } else {
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), len, buffer_.begin());
        upto_ = len;
    }
}

int64_t BufferedChecksum::get_value() {
    flush();
    return checksum_->get_value();
}

void BufferedChecksum::reset() {
    flush();
    checksum_->reset();
}

void BufferedChecksum::update_short(int16_t value) {
    const auto bytes = to_le_bytes(value);
    update_bytes(std::span<const uint8_t>(bytes), 0, bytes.size());
}

void BufferedChecksum::update_int(int32_t value) {
    const auto bytes = to_le_bytes(value);
    update_bytes(std::span<const uint8_t>(bytes), 0, bytes.size());
}

void BufferedChecksum::update_long(int64_t value) {
    const auto bytes = to_le_bytes(value);
    update_bytes(std::span<const uint8_t>(bytes), 0, bytes.size());
}

void BufferedChecksum::update_longs(std::span<const int64_t> values, size_t offset, size_t len) {
    debug_check(offset + len <= values.size());
    // rlucene keeps the direct path when the buffer cannot hold a whole long.
    if (buffer_.size() < sizeof(int64_t)) {
        for (size_t i = offset; i < offset + len; ++i) {
            update_long(values[i]);
        }
        return;
    }
    if (upto_ > 0) {
        size_t remaining = ((buffer_.size() - upto_) / sizeof(int64_t));
        if (remaining > len) {
            remaining = len;
        }
        for (size_t i = 0; i < remaining; ++i) {
            update_long(values[offset + i]);
        }
        offset += remaining;
        len -= remaining;
        if (len == 0) {
            return;
        }
    }
    const size_t capacity = buffer_.size() / sizeof(int64_t);
    while (len > 0) {
        const size_t batch = std::min(capacity, len);
        for (size_t i = 0; i < batch; ++i) {
            update_long(values[offset + i]);
        }
        offset += batch;
        len -= batch;
    }
}

} // namespace lucene
