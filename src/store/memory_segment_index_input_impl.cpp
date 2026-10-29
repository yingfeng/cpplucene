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

module;

#include <cerrno>
#include <sys/mman.h>
#include <unistd.h>

module lucene_core:memory_segment_index_input.impl;

import :memory_segment_index_input;
import :types;
import :error;
import :bit_util;
import :data_input;
import :index_input;
import :random_access_input;

import std;

namespace lucene {

// ---------------- MemoryMapping ----------------

MemoryMapping::~MemoryMapping() {
    for (size_t i = 0; i < segments_.size(); ++i) {
        if (segments_[i] != nullptr) {
            ::munmap(segments_[i], segment_sizes_[i]);
        }
    }
}

VoidResult MemoryMapping::map(int fd, size_t size, size_t chunk_size) {
    segments_.clear();
    segment_sizes_.clear();
    size_ = 0;
    chunk_size_ = chunk_size == 0 ? DEFAULT_MAPPING_CHUNK_SIZE : chunk_size;
    // `mmap` requires a page aligned offset, so every chunk but the first must
    // start on a page boundary.
    const size_t page_size = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
    if (page_size > 0 && chunk_size_ < page_size) {
        chunk_size_ = page_size;
    } else if (page_size > 0 && chunk_size_ % page_size != 0) {
        chunk_size_ = ((chunk_size_ + page_size - 1) / page_size) * page_size;
    }
    if (size == 0) {
        return {};
    }
    if (fd < 0) {
        return err(LuceneError::illegal_argument("cannot map a file without a file descriptor"));
    }

    const size_t chunks = (size + chunk_size_ - 1) / chunk_size_;
    segments_.reserve(chunks);
    segment_sizes_.reserve(chunks);
    for (size_t i = 0; i < chunks; ++i) {
        const size_t chunk_offset = i * chunk_size_;
        const size_t chunk_length = std::min(chunk_size_, size - chunk_offset);
        void *address = ::mmap(nullptr,
                               chunk_length,
                               PROT_READ,
                               MAP_SHARED,
                               fd,
                               static_cast<off_t>(chunk_offset));
        if (address == MAP_FAILED) {
            const std::string message =
                std::format("mmap of {} bytes at offset {} failed: {}", chunk_length, chunk_offset, std::strerror(errno));
            // Unmap whatever succeeded so the object stays consistent.
            for (size_t j = 0; j < segments_.size(); ++j) {
                ::munmap(segments_[j], segment_sizes_[j]);
            }
            segments_.clear();
            segment_sizes_.clear();
            return err(LuceneError::io(message));
        }
        segments_.push_back(static_cast<uint8_t *>(address));
        segment_sizes_.push_back(chunk_length);
    }
    size_ = size;
    return {};
}

void MemoryMapping::read(size_t pos, std::span<uint8_t> dst) const noexcept {
    size_t done = 0;
    while (done < dst.size()) {
        const size_t chunk = pos / chunk_size_;
        const size_t within = pos - (chunk * chunk_size_);
        const size_t available = segment_sizes_[chunk] - within;
        const size_t take = std::min(available, dst.size() - done);
        std::memcpy(dst.data() + done, segments_[chunk] + within, take);
        done += take;
        pos += take;
    }
}

void MemoryMapping::prefetch(size_t pos, size_t len) const noexcept {
    if (len == 0 || pos >= size_) {
        return;
    }
    const size_t end = std::min(size_, pos + len);
    size_t cursor = pos;
    while (cursor < end) {
        const size_t chunk = cursor / chunk_size_;
        const size_t within = cursor - (chunk * chunk_size_);
        const size_t available = std::min(segment_sizes_[chunk] - within, end - cursor);
        // Advisory only; failures are not actionable.
        std::ignore = ::madvise(segments_[chunk] + within,
                                available,
                                MADV_WILLNEED);
        cursor += available;
    }
}

// ---------------- MemorySegmentIndexInput ----------------

MemorySegmentIndexInput::MemorySegmentIndexInput(std::shared_ptr<MemoryMapping> mapping,
                                                 size_t offset,
                                                 size_t length,
                                                 std::string description)
    : mapping_(std::move(mapping)), offset_(offset), length_(length) {
    set_description(std::move(description));
}

VoidResult MemorySegmentIndexInput::check_range(size_t pos, size_t num_bytes) const {
    if (pos > length_ || num_bytes > length_ - pos) {
        return err(LuceneError::eof(std::format("read past slice end: {} (pos={} requested={} length={})",
                                                description(),
                                                pos,
                                                num_bytes,
                                                length_)));
    }
    if (offset_ + pos > mapping_->size() || num_bytes > mapping_->size() - (offset_ + pos)) {
        return err(LuceneError::eof(std::format("read past mapped region: {} (pos={} requested={} file_length={})",
                                                description(),
                                                offset_ + pos,
                                                num_bytes,
                                                mapping_->size())));
    }
    return {};
}

VoidResult MemorySegmentIndexInput::seek(size_t pos) {
    if (pos > length_) {
        return err(LuceneError::illegal_argument(
            std::format("seek target {} is beyond the input length {}", pos, length_)));
    }
    pos_ = pos;
    return {};
}

Result<uint8_t> MemorySegmentIndexInput::read_byte() {
    if (auto r = check_range(pos_, 1); !r) {
        return std::unexpected(std::move(r.error()));
    }
    uint8_t value = 0;
    mapping_->read(offset_ + pos_, std::span<uint8_t>(&value, 1));
    ++pos_;
    return value;
}

VoidResult MemorySegmentIndexInput::read_bytes(std::span<uint8_t> b) {
    if (b.empty()) {
        return {};
    }
    if (auto r = check_range(pos_, b.size()); !r) {
        return r;
    }
    mapping_->read(offset_ + pos_, b);
    pos_ += b.size();
    return {};
}

VoidResult MemorySegmentIndexInput::skip_bytes(int64_t num_bytes) {
    if (num_bytes < 0) {
        return err(LuceneError::illegal_argument(std::format("num_bytes must be >= 0, got {}", num_bytes)));
    }
    const size_t target = pos_ + static_cast<size_t>(num_bytes);
    if (target > length_) {
        return err(LuceneError::eof(std::format("skip past slice end: {} (pos={} skip={} length={})",
                                                description(),
                                                pos_,
                                                num_bytes,
                                                length_)));
    }
    pos_ = target;
    return {};
}

Result<int16_t> MemorySegmentIndexInput::read_short() {
    std::array<uint8_t, BitUtil::SHORT_BYTES> bytes{};
    if (auto r = read_bytes(bytes); !r) {
        return std::unexpected(std::move(r.error()));
    }
    return BitUtil::get_i16_le(bytes, 0);
}

Result<int32_t> MemorySegmentIndexInput::read_int() {
    std::array<uint8_t, BitUtil::INT_BYTES> bytes{};
    if (auto r = read_bytes(bytes); !r) {
        return std::unexpected(std::move(r.error()));
    }
    return BitUtil::get_i32_le(bytes, 0);
}

Result<int64_t> MemorySegmentIndexInput::read_long() {
    std::array<uint8_t, BitUtil::LONG_BYTES> bytes{};
    if (auto r = read_bytes(bytes); !r) {
        return std::unexpected(std::move(r.error()));
    }
    return BitUtil::get_i64_le(bytes, 0);
}

VoidResult MemorySegmentIndexInput::read_ints(std::span<int32_t> dst, size_t offset, size_t len) {
    return read_ints_bulk(*this, dst, offset, len);
}

Result<std::unique_ptr<IndexInput>> MemorySegmentIndexInput::slice(std::string_view slice_description,
                                                                  size_t offset,
                                                                  size_t length) const {
    if (offset > length_ || length > length_ - offset) {
        return err(LuceneError::illegal_argument(std::format("slice [{}, {}) is out of bounds of length {}",
                                                             offset,
                                                             offset + length,
                                                             length_)));
    }
    std::unique_ptr<IndexInput> sliced(new MemorySegmentIndexInput(
        mapping_, offset_ + offset, length, std::string(slice_description)));
    return sliced;
}

Result<std::unique_ptr<RandomAccessInput>> MemorySegmentIndexInput::random_access_slice(size_t offset,
                                                                                       size_t length) const {
    if (offset > length_ || length > length_ - offset) {
        return err(LuceneError::illegal_argument(std::format("random access slice [{}, {}) is out of bounds of length {}",
                                                             offset,
                                                             offset + length,
                                                             length_)));
    }
    std::unique_ptr<RandomAccessInput> view(
        new MemorySegmentRandomAccessInput(mapping_, offset_ + offset, length));
    return view;
}

Result<std::unique_ptr<IndexInput>> MemorySegmentIndexInput::clone() const {
    std::unique_ptr<IndexInput> copy(
        new MemorySegmentIndexInput(mapping_, offset_, length_, description()));
    static_cast<MemorySegmentIndexInput *>(copy.get())->pos_ = pos_;
    return copy;
}

VoidResult MemorySegmentIndexInput::prefetch(size_t pos, size_t len) {
    if (pos > length_) {
        return err(LuceneError::illegal_argument(
            std::format("prefetch at {} is beyond the input length {}", pos, length_)));
    }
    mapping_->prefetch(offset_ + pos, std::min(len, length_ - pos));
    return {};
}

VoidResult MemorySegmentIndexInput::close() { return {}; }

std::string MemorySegmentIndexInput::to_string() const {
    return std::format("MemorySegmentIndexInput({})", description());
}

Result<uint8_t> MemorySegmentIndexInput::read_byte(size_t pos) const {
    if (auto r = check_range(pos, 1); !r) {
        return std::unexpected(std::move(r.error()));
    }
    uint8_t value = 0;
    mapping_->read(offset_ + pos, std::span<uint8_t>(&value, 1));
    return value;
}

Result<int16_t> MemorySegmentIndexInput::read_short(size_t pos) const {
    std::array<uint8_t, BitUtil::SHORT_BYTES> bytes{};
    if (auto r = check_range(pos, bytes.size()); !r) {
        return std::unexpected(std::move(r.error()));
    }
    mapping_->read(offset_ + pos, bytes);
    return BitUtil::get_i16_le(bytes, 0);
}

Result<int32_t> MemorySegmentIndexInput::read_int(size_t pos) const {
    std::array<uint8_t, BitUtil::INT_BYTES> bytes{};
    if (auto r = check_range(pos, bytes.size()); !r) {
        return std::unexpected(std::move(r.error()));
    }
    mapping_->read(offset_ + pos, bytes);
    return BitUtil::get_i32_le(bytes, 0);
}

Result<int64_t> MemorySegmentIndexInput::read_long(size_t pos) const {
    std::array<uint8_t, BitUtil::LONG_BYTES> bytes{};
    if (auto r = check_range(pos, bytes.size()); !r) {
        return std::unexpected(std::move(r.error()));
    }
    mapping_->read(offset_ + pos, bytes);
    return BitUtil::get_i64_le(bytes, 0);
}

VoidResult MemorySegmentIndexInput::read_bytes(size_t pos, std::span<uint8_t> bytes) const {
    if (auto r = check_range(pos, bytes.size()); !r) {
        return r;
    }
    mapping_->read(offset_ + pos, bytes);
    return {};
}

// ---------------- MemorySegmentRandomAccessInput ----------------

MemorySegmentRandomAccessInput::MemorySegmentRandomAccessInput(std::shared_ptr<MemoryMapping> mapping,
                                                               size_t offset,
                                                               size_t length)
    : mapping_(std::move(mapping)), offset_(offset), length_(length) {}

VoidResult MemorySegmentRandomAccessInput::check_range(size_t pos, size_t num_bytes) const {
    if (pos > length_ || num_bytes > length_ - pos) {
        return err(LuceneError::eof(std::format("read past slice end: pos={} requested={} length={}",
                                                pos,
                                                num_bytes,
                                                length_)));
    }
    return {};
}

Result<uint8_t> MemorySegmentRandomAccessInput::read_byte(size_t pos) const {
    if (auto r = check_range(pos, 1); !r) {
        return std::unexpected(std::move(r.error()));
    }
    uint8_t value = 0;
    mapping_->read(offset_ + pos, std::span<uint8_t>(&value, 1));
    return value;
}

Result<int16_t> MemorySegmentRandomAccessInput::read_short(size_t pos) const {
    std::array<uint8_t, BitUtil::SHORT_BYTES> bytes{};
    if (auto r = check_range(pos, bytes.size()); !r) {
        return std::unexpected(std::move(r.error()));
    }
    mapping_->read(offset_ + pos, bytes);
    return BitUtil::get_i16_le(bytes, 0);
}

Result<int32_t> MemorySegmentRandomAccessInput::read_int(size_t pos) const {
    std::array<uint8_t, BitUtil::INT_BYTES> bytes{};
    if (auto r = check_range(pos, bytes.size()); !r) {
        return std::unexpected(std::move(r.error()));
    }
    mapping_->read(offset_ + pos, bytes);
    return BitUtil::get_i32_le(bytes, 0);
}

Result<int64_t> MemorySegmentRandomAccessInput::read_long(size_t pos) const {
    std::array<uint8_t, BitUtil::LONG_BYTES> bytes{};
    if (auto r = check_range(pos, bytes.size()); !r) {
        return std::unexpected(std::move(r.error()));
    }
    mapping_->read(offset_ + pos, bytes);
    return BitUtil::get_i64_le(bytes, 0);
}

VoidResult MemorySegmentRandomAccessInput::read_bytes(size_t pos, std::span<uint8_t> bytes) const {
    if (auto r = check_range(pos, bytes.size()); !r) {
        return r;
    }
    mapping_->read(offset_ + pos, bytes);
    return {};
}

} // namespace lucene
