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

module lucene_core:buffered_index_input.impl;

import :buffered_index_input;
import :types;
import :error;
import :bit_util;
import :data_input;
import :index_input;
import :io_context;
import :random_access_input;

import std;

namespace lucene {

// ---------------- BufferedIndexInput ----------------

BufferedIndexInput::BufferedIndexInput(std::unique_ptr<IndexInput> sub,
                                       std::unique_ptr<RandomAccessInput> source,
                                       std::string resource_description,
                                       size_t buffer_size,
                                       size_t file_length)
    : sub_(std::move(sub)),
      source_(std::move(source)),
      buffer_size_(buffer_size),
      file_length_(file_length),
      buffer_(buffer_size) {
    set_description(std::move(resource_description));
}

BufferedIndexInput::~BufferedIndexInput() = default;

size_t BufferedIndexInput::buffer_size_for(const IOContext &context) {
    return context.context() == IOContextType::Merge ? MERGE_BUFFER_SIZE : BUFFER_SIZE;
}

VoidResult BufferedIndexInput::check_buffer_size(size_t buffer_size) {
    if (buffer_size < MIN_BUFFER_SIZE) {
        return err(LuceneError::illegal_argument(
            std::format("buffer_size must be at least MIN_BUFFER_SIZE ({}) but was {}", MIN_BUFFER_SIZE, buffer_size)));
    }
    return {};
}

Result<size_t> BufferedIndexInput::length() const { return sub_->length(); }

VoidResult BufferedIndexInput::seek(size_t pos) {
    if (pos > file_length_) {
        return err(LuceneError::illegal_argument(
            std::format("seek target {} is beyond the input length {}", pos, file_length_)));
    }
    pos_ = pos;
    if (!(pos >= buffer_start_ && pos - buffer_start_ < length_)) {
        // The cache no longer helps; keep the window start in sync so the next
        // fill reads from `pos` instead of the stale window.
        buffer_start_ = pos;
        length_ = 0;
    }
    return {};
}

VoidResult BufferedIndexInput::fill_at(size_t pos) const {
    const size_t want = std::min(buffer_size_, file_length_ - pos);
    if (want == 0) {
        return err(LuceneError::eof(std::format("read past EOF: {}", description())));
    }
    if (auto r = source_->read_bytes(pos, std::span<uint8_t>(buffer_.data(), want)); !r) {
        return r;
    }
    buffer_start_ = pos;
    length_ = want;
    return {};
}

VoidResult BufferedIndexInput::read_at(size_t pos, std::span<uint8_t> dst) const {
    if (dst.empty()) {
        return {};
    }
    if (pos > file_length_ || dst.size() > file_length_ - pos) {
        return err(LuceneError::eof(std::format("read past EOF: {} (pos={} requested={} length={})",
                                                description(),
                                                pos,
                                                dst.size(),
                                                file_length_)));
    }

    size_t done = 0;
    if (pos >= buffer_start_ && pos - buffer_start_ < length_) {
        const size_t offset = pos - buffer_start_;
        const size_t take = std::min(length_ - offset, dst.size());
        std::memcpy(dst.data(), buffer_.data() + offset, take);
        pos += take;
        done = take;
    }
    const size_t remaining = dst.size() - done;
    if (remaining == 0) {
        return {};
    }

    if (remaining < buffer_size_) {
        if (auto r = fill_at(pos); !r) {
            return r;
        }
        if (length_ < remaining) {
            return err(LuceneError::eof(std::format("read past EOF: {} (expected {} got {})",
                                                    description(),
                                                    remaining,
                                                    length_)));
        }
        std::memcpy(dst.data() + done, buffer_.data(), remaining);
        return {};
    }

    // A request at least as wide as the window bypasses the cache: buffering it
    // would only add a copy.
    if (auto r = source_->read_bytes(pos, dst.subspan(done)); !r) {
        return r;
    }
    buffer_start_ = pos + remaining;
    length_ = 0;
    return {};
}

VoidResult BufferedIndexInput::read_at_locked(size_t pos, std::span<uint8_t> dst) const {
    std::lock_guard<std::mutex> guard(mutex_);
    return read_at(pos, dst);
}

Result<uint8_t> BufferedIndexInput::read_byte() {
    uint8_t value = 0;
    if (auto r = read_at(pos_, std::span<uint8_t>(&value, 1)); !r) {
        return std::unexpected(std::move(r.error()));
    }
    ++pos_;
    return value;
}

VoidResult BufferedIndexInput::read_bytes(std::span<uint8_t> b) {
    if (b.empty()) {
        return {};
    }
    if (auto r = read_at(pos_, b); !r) {
        return r;
    }
    pos_ += b.size();
    return {};
}

VoidResult BufferedIndexInput::read_bytes_with_buffer(std::span<uint8_t> b, bool use_buffer) {
    if (use_buffer) {
        return read_bytes(b);
    }
    if (b.empty()) {
        return {};
    }
    if (pos_ > file_length_ || b.size() > file_length_ - pos_) {
        return err(LuceneError::eof(std::format("read past EOF: {} (pos={} requested={} length={})",
                                                description(),
                                                pos_,
                                                b.size(),
                                                file_length_)));
    }
    if (auto r = source_->read_bytes(pos_, b); !r) {
        return r;
    }
    pos_ += b.size();
    return {};
}

VoidResult BufferedIndexInput::skip_bytes(int64_t num_bytes) {
    if (num_bytes < 0) {
        return err(LuceneError::illegal_argument(std::format("num_bytes must be >= 0, got {}", num_bytes)));
    }
    const size_t target = pos_ + static_cast<size_t>(num_bytes);
    if (target > file_length_) {
        return err(LuceneError::eof(std::format("skip past EOF: {} (pos={} skip={} length={})",
                                                description(),
                                                pos_,
                                                num_bytes,
                                                file_length_)));
    }
    pos_ = target;
    return {};
}

Result<int16_t> BufferedIndexInput::read_short() {
    std::array<uint8_t, BitUtil::SHORT_BYTES> bytes{};
    if (auto r = read_at(pos_, bytes); !r) {
        return std::unexpected(std::move(r.error()));
    }
    pos_ += bytes.size();
    return BitUtil::get_i16_le(bytes, 0);
}

Result<int32_t> BufferedIndexInput::read_int() {
    std::array<uint8_t, BitUtil::INT_BYTES> bytes{};
    if (auto r = read_at(pos_, bytes); !r) {
        return std::unexpected(std::move(r.error()));
    }
    pos_ += bytes.size();
    return BitUtil::get_i32_le(bytes, 0);
}

Result<int64_t> BufferedIndexInput::read_long() {
    std::array<uint8_t, BitUtil::LONG_BYTES> bytes{};
    if (auto r = read_at(pos_, bytes); !r) {
        return std::unexpected(std::move(r.error()));
    }
    pos_ += bytes.size();
    return BitUtil::get_i64_le(bytes, 0);
}

Result<std::unique_ptr<IndexInput>> BufferedIndexInput::slice(std::string_view slice_description,
                                                             size_t offset,
                                                             size_t length) const {
    if (offset > file_length_ || length > file_length_ - offset) {
        return err(LuceneError::illegal_argument(std::format("slice [{}, {}) is out of bounds of length {}",
                                                             offset,
                                                             offset + length,
                                                             file_length_)));
    }
    auto sliced = sub_->slice(slice_description, offset, length);
    if (!sliced) {
        return std::unexpected(std::move(sliced.error()));
    }
    auto wrapped = open_buffered_with_buffer_size(
        std::move(sliced.value()), std::string(slice_description), buffer_size_);
    if (!wrapped) {
        return std::unexpected(std::move(wrapped.error()));
    }
    return std::unique_ptr<IndexInput>(std::move(wrapped.value()));
}

Result<std::unique_ptr<RandomAccessInput>> BufferedIndexInput::random_access_slice(size_t offset,
                                                                                  size_t length) const {
    if (offset > file_length_ || length > file_length_ - offset) {
        return err(LuceneError::illegal_argument(std::format("random access slice [{}, {}) is out of bounds of length {}",
                                                             offset,
                                                             offset + length,
                                                             file_length_)));
    }
    auto sliced = sub_->slice("random_access_slice", offset, length);
    if (!sliced) {
        return std::unexpected(std::move(sliced.error()));
    }
    auto wrapped = open_buffered_with_buffer_size(
        std::move(sliced.value()), "random_access_slice", buffer_size_);
    if (!wrapped) {
        return std::unexpected(std::move(wrapped.error()));
    }
    // BufferedIndexInput is a RandomAccessInput, so the conversion is an upcast.
    return std::unique_ptr<RandomAccessInput>(std::move(wrapped.value()));
}

Result<std::unique_ptr<IndexInput>> BufferedIndexInput::clone() const {
    auto sub_clone = sub_->clone();
    if (!sub_clone) {
        return std::unexpected(std::move(sub_clone.error()));
    }
    auto cloned = open_buffered_with_buffer_size(std::move(sub_clone.value()), description(), buffer_size_);
    if (!cloned) {
        return std::unexpected(std::move(cloned.error()));
    }
    std::unique_ptr<BufferedIndexInput> copy = std::move(cloned.value());
    copy->pos_ = pos_;
    copy->buffer_start_ = pos_;
    copy->length_ = 0;
    return std::unique_ptr<IndexInput>(std::move(copy));
}

VoidResult BufferedIndexInput::prefetch(size_t pos, size_t len) { return sub_->prefetch(pos, len); }

VoidResult BufferedIndexInput::close() { return sub_->close(); }

std::string BufferedIndexInput::to_string() const {
    return std::format("BufferedIndexInput({})", description());
}

// ---- RandomAccessInput ----

Result<uint8_t> BufferedIndexInput::read_byte(size_t pos) const {
    uint8_t value = 0;
    if (auto r = read_at_locked(pos, std::span<uint8_t>(&value, 1)); !r) {
        return std::unexpected(std::move(r.error()));
    }
    return value;
}

Result<int16_t> BufferedIndexInput::read_short(size_t pos) const {
    std::array<uint8_t, BitUtil::SHORT_BYTES> bytes{};
    if (auto r = read_at_locked(pos, bytes); !r) {
        return std::unexpected(std::move(r.error()));
    }
    return BitUtil::get_i16_le(bytes, 0);
}

Result<int32_t> BufferedIndexInput::read_int(size_t pos) const {
    std::array<uint8_t, BitUtil::INT_BYTES> bytes{};
    if (auto r = read_at_locked(pos, bytes); !r) {
        return std::unexpected(std::move(r.error()));
    }
    return BitUtil::get_i32_le(bytes, 0);
}

Result<int64_t> BufferedIndexInput::read_long(size_t pos) const {
    std::array<uint8_t, BitUtil::LONG_BYTES> bytes{};
    if (auto r = read_at_locked(pos, bytes); !r) {
        return std::unexpected(std::move(r.error()));
    }
    return BitUtil::get_i64_le(bytes, 0);
}

VoidResult BufferedIndexInput::read_bytes(size_t pos, std::span<uint8_t> bytes) const {
    return read_at_locked(pos, bytes);
}

// ---------------- factories ----------------

Result<std::unique_ptr<BufferedIndexInput>> open_buffered_with_buffer_size(
    std::unique_ptr<IndexInput> sub,
    std::string resource_description,
    size_t buffer_size) {
    if (auto check = BufferedIndexInput::check_buffer_size(buffer_size); !check) {
        return std::unexpected(std::move(check.error()));
    }
    if (!sub) {
        return err(LuceneError::illegal_argument("buffered input needs a non-null sub input"));
    }
    auto length = sub->length();
    if (!length) {
        return std::unexpected(std::move(length.error()));
    }
    auto source = sub->random_access_slice(0, length.value());
    if (!source) {
        return std::unexpected(std::move(source.error()));
    }
    std::unique_ptr<BufferedIndexInput> buffered(new BufferedIndexInput(std::move(sub),
                                                                       std::move(source.value()),
                                                                       std::move(resource_description),
                                                                       buffer_size,
                                                                       length.value()));
    return buffered;
}

Result<std::unique_ptr<BufferedIndexInput>> open_buffered(std::unique_ptr<IndexInput> sub,
                                                          std::string resource_description) {
    return open_buffered_with_buffer_size(
        std::move(sub), std::move(resource_description), BufferedIndexInput::BUFFER_SIZE);
}

Result<std::unique_ptr<BufferedIndexInput>> open_buffered_with_context(std::unique_ptr<IndexInput> sub,
                                                                     std::string resource_description,
                                                                     const IOContext &context) {
    return open_buffered_with_buffer_size(std::move(sub),
                                          std::move(resource_description),
                                          BufferedIndexInput::buffer_size_for(context));
}

} // namespace lucene
