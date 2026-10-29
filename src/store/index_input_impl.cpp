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

module lucene_core:index_input.impl;

import :index_input;
import :types;
import :debug;
import :core_helper;

import std;

namespace lucene {

// ---------------- IndexInput ----------------

VoidResult IndexInput::skip_bytes(int64_t num_bytes) {
    if (num_bytes < 0) {
        return err(LuceneError::illegal_argument(std::format("num_bytes must be >= 0, got {}", num_bytes)));
    }
    auto pointer = get_file_pointer();
    if (!pointer) {
        return std::unexpected(std::move(pointer.error()));
    }
    return seek(pointer.value() + static_cast<size_t>(num_bytes));
}

Result<std::unique_ptr<IndexInput>> IndexInput::slice(std::string_view /*slice_description*/,
                                                      size_t /*offset*/,
                                                      size_t /*length*/) const {
    return err(LuceneError::unsupported_operation("this IndexInput does not support slicing"));
}

Result<std::unique_ptr<RandomAccessInput>> IndexInput::random_access_slice(size_t offset, size_t length) const {
    auto cloned = clone();
    if (!cloned) {
        return std::unexpected(std::move(cloned.error()));
    }
    return std::unique_ptr<RandomAccessInput>(
        std::make_unique<SeekReadRandomAccessInput>(std::move(cloned.value()), offset, length));
}

// ---------------- SlicedIndexInput ----------------

SlicedIndexInput::SlicedIndexInput(std::unique_ptr<IndexInput> parent,
                                   size_t offset,
                                   size_t length,
                                   std::string description)
    : parent_(std::move(parent)), offset_(offset), length_(length) {
    set_description(std::move(description));
}

VoidResult SlicedIndexInput::seek(size_t pos) {
    if (pos > length_) {
        return err(LuceneError::illegal_argument(
            std::format("seek target {} is beyond the slice length {}", pos, length_)));
    }
    pos_ = pos;
    return {};
}

VoidResult SlicedIndexInput::prepare_read() const {
    if (auto r = parent_->seek(offset_ + pos_); !r) {
        return r;
    }
    return {};
}

Result<uint8_t> SlicedIndexInput::read_byte() {
    if (pos_ >= length_) {
        return err(LuceneError::eof(std::format("read past slice end: pos={} length={}", pos_, length_)));
    }
    if (auto r = prepare_read(); !r) {
        return std::unexpected(std::move(r.error()));
    }
    auto value = parent_->read_byte();
    if (!value) {
        return std::unexpected(std::move(value.error()));
    }
    ++pos_;
    return value;
}

VoidResult SlicedIndexInput::read_bytes(std::span<uint8_t> b) {
    if (b.empty()) {
        return {};
    }
    if (pos_ + b.size() > length_) {
        return err(LuceneError::eof(
            std::format("read past slice end: pos={} requested={} length={}", pos_, b.size(), length_)));
    }
    if (auto r = prepare_read(); !r) {
        return r;
    }
    if (auto r = parent_->read_bytes(b); !r) {
        return r;
    }
    pos_ += b.size();
    return {};
}

VoidResult SlicedIndexInput::skip_bytes(int64_t num_bytes) {
    if (num_bytes < 0) {
        return err(LuceneError::illegal_argument(std::format("num_bytes must be >= 0, got {}", num_bytes)));
    }
    const size_t target = pos_ + static_cast<size_t>(num_bytes);
    if (target > length_) {
        return err(LuceneError::eof(std::format("skip past slice end: pos={} skip={} length={}",
                                                pos_,
                                                num_bytes,
                                                length_)));
    }
    pos_ = target;
    return {};
}

Result<int16_t> SlicedIndexInput::read_short() {
    if (pos_ + 2 > length_) {
        return err(LuceneError::eof(std::format("read past slice end: pos={} length={}", pos_, length_)));
    }
    if (auto r = prepare_read(); !r) {
        return std::unexpected(std::move(r.error()));
    }
    auto value = parent_->read_short();
    if (!value) {
        return std::unexpected(std::move(value.error()));
    }
    pos_ += 2;
    return value;
}

Result<int32_t> SlicedIndexInput::read_int() {
    if (pos_ + 4 > length_) {
        return err(LuceneError::eof(std::format("read past slice end: pos={} length={}", pos_, length_)));
    }
    if (auto r = prepare_read(); !r) {
        return std::unexpected(std::move(r.error()));
    }
    auto value = parent_->read_int();
    if (!value) {
        return std::unexpected(std::move(value.error()));
    }
    pos_ += 4;
    return value;
}

Result<int64_t> SlicedIndexInput::read_long() {
    if (pos_ + 8 > length_) {
        return err(LuceneError::eof(std::format("read past slice end: pos={} length={}", pos_, length_)));
    }
    if (auto r = prepare_read(); !r) {
        return std::unexpected(std::move(r.error()));
    }
    auto value = parent_->read_long();
    if (!value) {
        return std::unexpected(std::move(value.error()));
    }
    pos_ += 8;
    return value;
}

VoidResult SlicedIndexInput::read_ints(std::span<int32_t> dst, size_t offset, size_t len) {
    return read_ints_bulk(*this, dst, offset, len);
}

Result<std::unique_ptr<IndexInput>> SlicedIndexInput::slice(std::string_view slice_description,
                                                            size_t offset,
                                                            size_t length) const {
    if (offset + length > length_) {
        return err(LuceneError::illegal_argument(std::format("slice [{}, {}) is out of bounds of length {}",
                                                             offset,
                                                             offset + length,
                                                             length_)));
    }
    auto cloned = clone();
    if (!cloned) {
        return std::unexpected(std::move(cloned.error()));
    }
    return std::unique_ptr<IndexInput>(
        std::make_unique<SlicedIndexInput>(std::move(cloned.value()), offset, length, std::string(slice_description)));
}

Result<std::unique_ptr<RandomAccessInput>> SlicedIndexInput::random_access_slice(size_t offset, size_t length) const {
    if (offset + length > length_) {
        return err(LuceneError::illegal_argument(std::format("random access slice [{}, {}) is out of bounds of length {}",
                                                             offset,
                                                             offset + length,
                                                             length_)));
    }
    auto cloned = clone();
    if (!cloned) {
        return std::unexpected(std::move(cloned.error()));
    }
    return std::unique_ptr<RandomAccessInput>(std::make_unique<SeekReadRandomAccessInput>(
        std::move(cloned.value()), offset_ + offset, length));
}

Result<std::unique_ptr<IndexInput>> SlicedIndexInput::clone() const {
    // The clone keeps the same parent position semantics by cloning the parent
    // as well; `prepare_read` re-seeks before every read, so both copies stay
    // independent.
    auto parent_clone = parent_->clone();
    if (!parent_clone) {
        return std::unexpected(std::move(parent_clone.error()));
    }
    auto result = std::make_unique<SlicedIndexInput>(std::move(parent_clone.value()), offset_, length_, description());
    if (auto r = result->seek(pos_); !r) {
        return std::unexpected(std::move(r.error()));
    }
    return std::unique_ptr<IndexInput>(std::move(result));
}

VoidResult SlicedIndexInput::prefetch(size_t pos, size_t len) {
    if (pos + len > length_) {
        return err(LuceneError::illegal_argument(std::format("prefetch [{}, {}) is out of bounds of length {}",
                                                             pos,
                                                             pos + len,
                                                             length_)));
    }
    return parent_->prefetch(offset_ + pos, len);
}

VoidResult SlicedIndexInput::close() {
    if (parent_) {
        return parent_->close();
    }
    return {};
}

// ---------------- SeekReadRandomAccessInput ----------------

SeekReadRandomAccessInput::SeekReadRandomAccessInput(std::unique_ptr<IndexInput> input, size_t offset, size_t length)
    : input_(std::move(input)), offset_(offset), length_(length) {}

Result<size_t> SeekReadRandomAccessInput::check(size_t pos, size_t num_bytes) const {
    if (pos + num_bytes > length_) {
        return err(LuceneError::eof(std::format("read [{}, {}) out of bounds of length {}",
                                                pos,
                                                pos + num_bytes,
                                                length_)));
    }
    return pos;
}

Result<uint8_t> SeekReadRandomAccessInput::read_byte(size_t pos) const {
    if (auto r = check(pos, 1); !r) {
        return std::unexpected(std::move(r.error()));
    }
    std::lock_guard<std::mutex> guard(mutex_);
    if (auto r = input_->seek(offset_ + pos); !r) {
        return std::unexpected(std::move(r.error()));
    }
    return input_->read_byte();
}

Result<int16_t> SeekReadRandomAccessInput::read_short(size_t pos) const {
    if (auto r = check(pos, 2); !r) {
        return std::unexpected(std::move(r.error()));
    }
    std::lock_guard<std::mutex> guard(mutex_);
    if (auto r = input_->seek(offset_ + pos); !r) {
        return std::unexpected(std::move(r.error()));
    }
    return input_->read_short();
}

Result<int32_t> SeekReadRandomAccessInput::read_int(size_t pos) const {
    if (auto r = check(pos, 4); !r) {
        return std::unexpected(std::move(r.error()));
    }
    std::lock_guard<std::mutex> guard(mutex_);
    if (auto r = input_->seek(offset_ + pos); !r) {
        return std::unexpected(std::move(r.error()));
    }
    return input_->read_int();
}

Result<int64_t> SeekReadRandomAccessInput::read_long(size_t pos) const {
    if (auto r = check(pos, 8); !r) {
        return std::unexpected(std::move(r.error()));
    }
    std::lock_guard<std::mutex> guard(mutex_);
    if (auto r = input_->seek(offset_ + pos); !r) {
        return std::unexpected(std::move(r.error()));
    }
    return input_->read_long();
}

VoidResult SeekReadRandomAccessInput::read_bytes(size_t pos, std::span<uint8_t> bytes) const {
    if (auto r = check(pos, bytes.size()); !r) {
        return std::unexpected(std::move(r.error()));
    }
    std::lock_guard<std::mutex> guard(mutex_);
    if (auto r = input_->seek(offset_ + pos); !r) {
        return r;
    }
    return input_->read_bytes(bytes);
}

} // namespace lucene
