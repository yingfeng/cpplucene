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

module lucene_core:fst_store.impl;

import :fst_store;

import :data_input;
import :data_output;
import :debug;
import :error;
import :fst_bytes_reader;
import :types;

import std;

namespace lucene {

namespace {

/// Bounds check shared by both readers: `pos_` is relative to the slice.
bool in_bounds(int64_t position, size_t length) {
    return position >= 0 && static_cast<size_t>(position) < length;
}

} // namespace

// ---- OwnedBytesReader ----

OwnedBytesReader::OwnedBytesReader(std::shared_ptr<const std::vector<uint8_t>> bytes, size_t offset, size_t length)
    : bytes_(std::move(bytes)), offset_(offset), length_(length) {
    debug_check(offset_ + length_ <= bytes_->size(), "the reader slice must fit into the buffer");
}

Result<uint8_t> OwnedBytesReader::read_byte() {
    if (!in_bounds(pos_, length_)) {
        return err(LuceneError::eof("read past the end of the FST data"));
    }
    return (*bytes_)[offset_ + static_cast<size_t>(pos_++)];
}

VoidResult OwnedBytesReader::read_bytes(std::span<uint8_t> b) {
    if (pos_ < 0 || static_cast<size_t>(pos_) + b.size() > length_) {
        return err(LuceneError::eof("read past the end of the FST data"));
    }
    std::copy_n(bytes_->begin() + static_cast<std::ptrdiff_t>(offset_ + static_cast<size_t>(pos_)), b.size(),
                b.begin());
    pos_ += static_cast<int64_t>(b.size());
    return {};
}

VoidResult OwnedBytesReader::skip_bytes(int64_t num_bytes) {
    pos_ += num_bytes;
    return {};
}

// ---- SliceBytesReader ----

Result<uint8_t> SliceBytesReader::read_byte() {
    if (!in_bounds(pos_, bytes_.size())) {
        return err(LuceneError::eof("read past the end of the FST slice"));
    }
    return bytes_[static_cast<size_t>(pos_++)];
}

VoidResult SliceBytesReader::read_bytes(std::span<uint8_t> b) {
    if (pos_ < 0 || static_cast<size_t>(pos_) + b.size() > bytes_.size()) {
        return err(LuceneError::eof("read past the end of the FST slice"));
    }
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(pos_), b.size(), b.begin());
    pos_ += static_cast<int64_t>(b.size());
    return {};
}

VoidResult SliceBytesReader::skip_bytes(int64_t num_bytes) {
    pos_ += num_bytes;
    return {};
}

// ---- OnHeapFstStore ----

OnHeapFstStore::OnHeapFstStore(std::shared_ptr<const std::vector<uint8_t>> bytes) : bytes_(std::move(bytes)) {
    debug_check(bytes_ != nullptr, "an FST store needs a buffer");
}

OnHeapFstStore::OnHeapFstStore(std::vector<uint8_t> bytes)
    : bytes_(std::make_shared<const std::vector<uint8_t>>(std::move(bytes))) {}

Result<OnHeapFstStore> OnHeapFstStore::read_from(DataInput &input, int64_t num_bytes) {
    if (num_bytes < 0) {
        return err(LuceneError::corrupt_index("negative FST length: " + std::to_string(num_bytes)));
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(num_bytes));
    if (auto read = input.read_bytes(bytes); !read.has_value()) {
        return err(read.error());
    }
    return OnHeapFstStore(std::move(bytes));
}

std::unique_ptr<FstBytesReader> OnHeapFstStore::make_reader() const {
    return std::make_unique<OwnedBytesReader>(bytes_, 0, bytes_->size());
}

VoidResult OnHeapFstStore::write_to(DataOutput &out) const {
    return out.write_bytes(*bytes_);
}

int64_t OnHeapFstStore::ram_bytes_used() const { return static_cast<int64_t>(bytes_->size()); }

int64_t OnHeapFstStore::num_bytes() const { return static_cast<int64_t>(bytes_->size()); }

// ---- OffHeapFstStore ----

OffHeapFstStore::OffHeapFstStore(std::span<const uint8_t> bytes, size_t offset, size_t num_bytes)
    : bytes_(bytes), offset_(offset), length_(num_bytes) {
    debug_check(offset_ + length_ <= bytes_.size(), "the FST slice must fit into the buffer");
}

std::unique_ptr<FstBytesReader> OffHeapFstStore::make_reader() const {
    return std::make_unique<SliceBytesReader>(bytes_.subspan(offset_, length_));
}

VoidResult OffHeapFstStore::write_to(DataOutput &out) const {
    // The bytes already exist in the index file; rlucene rejects this too.
    (void)out;
    return err(LuceneError::unsupported_operation("write_to is not supported by OffHeapFstStore"));
}

int64_t OffHeapFstStore::ram_bytes_used() const { return 0; }

int64_t OffHeapFstStore::num_bytes() const { return static_cast<int64_t>(length_); }

} // namespace lucene
