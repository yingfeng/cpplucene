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

module lucene_core:byte_buffers_directory.impl;

import :byte_buffers_directory;
import :types;
import :error;
import :debug;
import :bit_util;
import :crc32;
import :core_helper;
import :directory;
import :index_input;
import :index_output;
import :io_context;
import :lock;
import :lock_factory;
import :random_access_input;

import std;

namespace lucene {

namespace {

using Buffer = std::shared_ptr<const std::vector<uint8_t>>;

/// A random-access view over an in-memory buffer.
class BufferRandomAccessInput : public RandomAccessInput {
public:
    BufferRandomAccessInput(Buffer buffer, size_t offset, size_t length)
        : buffer_(std::move(buffer)), offset_(offset), length_(length) {}

    [[nodiscard]] Result<uint8_t> read_byte(size_t pos) const override {
        if (pos >= length_) {
            return err(LuceneError::eof(std::format("read past buffer end: pos={} length={}", pos, length_)));
        }
        return (*buffer_)[offset_ + pos];
    }

    [[nodiscard]] Result<int16_t> read_short(size_t pos) const override {
        if (pos + BitUtil::SHORT_BYTES > length_) {
            return err(LuceneError::eof(std::format("read past buffer end: pos={} length={}", pos, length_)));
        }
        return BitUtil::get_i16_le(std::span<const uint8_t>(*buffer_), offset_ + pos);
    }

    [[nodiscard]] Result<int32_t> read_int(size_t pos) const override {
        if (pos + BitUtil::INT_BYTES > length_) {
            return err(LuceneError::eof(std::format("read past buffer end: pos={} length={}", pos, length_)));
        }
        return BitUtil::get_i32_le(std::span<const uint8_t>(*buffer_), offset_ + pos);
    }

    [[nodiscard]] Result<int64_t> read_long(size_t pos) const override {
        if (pos + BitUtil::LONG_BYTES > length_) {
            return err(LuceneError::eof(std::format("read past buffer end: pos={} length={}", pos, length_)));
        }
        return BitUtil::get_i64_le(std::span<const uint8_t>(*buffer_), offset_ + pos);
    }

    [[nodiscard]] VoidResult read_bytes(size_t pos, std::span<uint8_t> bytes) const override {
        if (pos + bytes.size() > length_) {
            return err(LuceneError::eof(
                std::format("read past buffer end: pos={} requested={} length={}", pos, bytes.size(), length_)));
        }
        std::copy_n(buffer_->begin() + static_cast<std::ptrdiff_t>(offset_ + pos), bytes.size(), bytes.begin());
        return {};
    }

    [[nodiscard]] Result<size_t> length() const override { return length_; }

private:
    Buffer buffer_;
    size_t offset_{0};
    size_t length_{0};
};

/// An [`IndexInput`] over an in-memory buffer.
class BufferIndexInput : public IndexInput {
public:
    BufferIndexInput(Buffer buffer, size_t offset, size_t length, std::string description)
        : buffer_(std::move(buffer)), offset_(offset), length_(length) {
        set_description(std::move(description));
    }

    [[nodiscard]] Result<size_t> get_file_pointer() const override { return pos_; }

    [[nodiscard]] VoidResult seek(size_t pos) override {
        if (pos > length_) {
            return err(LuceneError::illegal_argument(
                std::format("seek target {} is beyond the input length {}", pos, length_)));
        }
        pos_ = pos;
        return {};
    }

    [[nodiscard]] Result<size_t> length() const override { return length_; }

    [[nodiscard]] Result<uint8_t> read_byte() override {
        if (pos_ >= length_) {
            return err(LuceneError::eof(std::format("read past end: pos={} length={}", pos_, length_)));
        }
        return (*buffer_)[offset_ + pos_++];
    }

    [[nodiscard]] VoidResult read_bytes(std::span<uint8_t> b) override {
        if (pos_ + b.size() > length_) {
            return err(LuceneError::eof(
                std::format("read past end: pos={} requested={} length={}", pos_, b.size(), length_)));
        }
        std::copy_n(buffer_->begin() + static_cast<std::ptrdiff_t>(offset_ + pos_), b.size(), b.begin());
        pos_ += b.size();
        return {};
    }

    [[nodiscard]] VoidResult skip_bytes(int64_t num_bytes) override {
        if (num_bytes < 0) {
            return err(LuceneError::illegal_argument(std::format("num_bytes must be >= 0, got {}", num_bytes)));
        }
        const size_t target = pos_ + static_cast<size_t>(num_bytes);
        if (target > length_) {
            return err(LuceneError::eof(
                std::format("skip past end: pos={} skip={} length={}", pos_, num_bytes, length_)));
        }
        pos_ = target;
        return {};
    }

    [[nodiscard]] Result<int16_t> read_short() override {
        if (pos_ + BitUtil::SHORT_BYTES > length_) {
            return err(LuceneError::eof(std::format("read past end: pos={} length={}", pos_, length_)));
        }
        const int16_t value = BitUtil::get_i16_le(std::span<const uint8_t>(*buffer_), offset_ + pos_);
        pos_ += BitUtil::SHORT_BYTES;
        return value;
    }

    [[nodiscard]] Result<int32_t> read_int() override {
        if (pos_ + BitUtil::INT_BYTES > length_) {
            return err(LuceneError::eof(std::format("read past end: pos={} length={}", pos_, length_)));
        }
        const int32_t value = BitUtil::get_i32_le(std::span<const uint8_t>(*buffer_), offset_ + pos_);
        pos_ += BitUtil::INT_BYTES;
        return value;
    }

    [[nodiscard]] Result<int64_t> read_long() override {
        if (pos_ + BitUtil::LONG_BYTES > length_) {
            return err(LuceneError::eof(std::format("read past end: pos={} length={}", pos_, length_)));
        }
        const int64_t value = BitUtil::get_i64_le(std::span<const uint8_t>(*buffer_), offset_ + pos_);
        pos_ += BitUtil::LONG_BYTES;
        return value;
    }

    [[nodiscard]] Result<std::unique_ptr<IndexInput>> slice(std::string_view slice_description,
                                                            size_t offset,
                                                            size_t length) const override {
        if (offset + length > length_) {
            return err(LuceneError::illegal_argument(std::format("slice [{}, {}) is out of bounds of length {}",
                                                                 offset,
                                                                 offset + length,
                                                                 length_)));
        }
        return std::unique_ptr<IndexInput>(std::make_unique<BufferIndexInput>(
            buffer_, offset_ + offset, length, std::string(slice_description)));
    }

    [[nodiscard]] Result<std::unique_ptr<RandomAccessInput>> random_access_slice(size_t offset,
                                                                                size_t length) const override {
        if (offset + length > length_) {
            return err(LuceneError::illegal_argument(std::format("slice [{}, {}) is out of bounds of length {}",
                                                                 offset,
                                                                 offset + length,
                                                                 length_)));
        }
        return std::unique_ptr<RandomAccessInput>(
            std::make_unique<BufferRandomAccessInput>(buffer_, offset_ + offset, length));
    }

    [[nodiscard]] Result<std::unique_ptr<IndexInput>> clone() const override {
        auto copy = std::make_unique<BufferIndexInput>(buffer_, offset_, length_, description());
        copy->pos_ = pos_;
        return std::unique_ptr<IndexInput>(std::move(copy));
    }

private:
    Buffer buffer_;
    size_t offset_{0};
    size_t length_{0};
    size_t pos_{0};
};

/// An [`IndexOutput`] that appends to a growable buffer and commits it to the
/// directory on `close()`.
class BufferIndexOutput : public IndexOutput {
public:
    using CommitFn = std::function<void(std::vector<uint8_t>)>;

    BufferIndexOutput(std::string name, CommitFn commit) : name_(std::move(name)), commit_(std::move(commit)) {}

    [[nodiscard]] Result<size_t> get_file_pointer() const override { return bytes_.size(); }
    [[nodiscard]] uint64_t get_checksum() override { return crc_.get_value(); }
    [[nodiscard]] std::string_view get_name() const override { return name_; }

    [[nodiscard]] VoidResult write_byte(uint8_t b) override {
        bytes_.push_back(b);
        crc_.update(b);
        return {};
    }

    [[nodiscard]] VoidResult write_bytes(std::span<const uint8_t> b, size_t offset, size_t length) override {
        if (auto check = CoreHelper::check_from_index_size(offset, length, b.size()); !check) {
            return std::unexpected(std::move(check.error()));
        }
        const auto source = b.subspan(offset, length);
        bytes_.insert(bytes_.end(), source.begin(), source.end());
        crc_.update(source);
        return {};
    }

    [[nodiscard]] VoidResult close() override {
        if (closed_) {
            return {};
        }
        closed_ = true;
        commit_(std::move(bytes_));
        bytes_.clear();
        return {};
    }

private:
    std::string name_;
    CommitFn commit_;
    std::vector<uint8_t> bytes_;
    Crc32 crc_;
    bool closed_{false};
};

} // namespace

// ---------------- ByteBuffersDirectory ----------------

ByteBuffersDirectory::ByteBuffersDirectory()
    : lock_factory_(std::make_unique<SingleInstanceLockFactory>()) {}

ByteBuffersDirectory::ByteBuffersDirectory(std::unique_ptr<LockFactory> lock_factory)
    : lock_factory_(std::move(lock_factory)) {
    if (!lock_factory_) {
        lock_factory_ = std::make_unique<SingleInstanceLockFactory>();
    }
}

Result<std::vector<std::string>> ByteBuffersDirectory::list_all() const {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<std::string> names;
    names.reserve(files_.size());
    for (const auto &[name, buffer] : files_) {
        names.push_back(name);
    }
    // std::map already keeps the names ordered.
    return names;
}

VoidResult ByteBuffersDirectory::delete_file(std::string_view name) {
    std::lock_guard<std::mutex> guard(mutex_);
    // Deleting a missing file is a no-op, matching Lucene's `Directory`.
    files_.erase(std::string(name));
    return {};
}

Result<size_t> ByteBuffersDirectory::file_length(std::string_view name) const {
    auto buffer = buffer_for(name);
    if (!buffer) {
        return std::unexpected(std::move(buffer.error()));
    }
    return buffer.value()->size();
}

Result<std::unique_ptr<IndexOutput>> ByteBuffersDirectory::create_output(std::string_view name,
                                                                          const IOContext & /*context*/) {
    std::string file_name(name);
    return std::unique_ptr<IndexOutput>(std::make_unique<BufferIndexOutput>(
        file_name, [this, file_name](std::vector<uint8_t> bytes) { commit(file_name, std::move(bytes)); }));
}

Result<CreatedOutput> ByteBuffersDirectory::create_temp_output(std::string_view prefix,
                                                               std::string_view suffix,
                                                               const IOContext &context) {
    auto name = new_file_name(prefix, suffix);
    auto output = create_output(name, context);
    if (!output) {
        return std::unexpected(std::move(output.error()));
    }
    return CreatedOutput{std::move(name), std::move(output.value())};
}

VoidResult ByteBuffersDirectory::sync(std::span<const std::string> /*names*/) { return {}; }

VoidResult ByteBuffersDirectory::rename(std::string_view source, std::string_view dest) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = files_.find(std::string(source));
    if (it == files_.end()) {
        return err(LuceneError::not_such_file(std::format("file {} does not exist", source)));
    }
    // Copy the buffer out instead of permuting the node key: mutating a
    // `std::map` node key in place is not portable across implementations.
    auto buffer = it->second;
    files_.erase(it);
    files_.insert_or_assign(std::string(dest), std::move(buffer));
    return {};
}

Result<std::unique_ptr<IndexInput>> ByteBuffersDirectory::open_input(std::string_view name,
                                                                     const IOContext & /*context*/) const {
    auto buffer = buffer_for(name);
    if (!buffer) {
        return std::unexpected(std::move(buffer.error()));
    }
    return std::unique_ptr<IndexInput>(
        std::make_unique<BufferIndexInput>(buffer.value(), 0, buffer.value()->size(), std::string(name)));
}

Result<std::unique_ptr<Lock>> ByteBuffersDirectory::obtain_lock(std::string_view name) {
    return lock_factory_->obtain_lock(name);
}

size_t ByteBuffersDirectory::file_count() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return files_.size();
}

size_t ByteBuffersDirectory::ram_bytes_used() const {
    std::lock_guard<std::mutex> guard(mutex_);
    size_t total = 0;
    for (const auto &[name, buffer] : files_) {
        total += buffer->size();
    }
    return total;
}

Result<Buffer> ByteBuffersDirectory::buffer_for(std::string_view name) const {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = files_.find(name);
    if (it == files_.end()) {
        return err(LuceneError::not_such_file(std::format("file {} does not exist", name)));
    }
    return it->second;
}

void ByteBuffersDirectory::commit(std::string name, std::vector<uint8_t> bytes) {
    auto buffer = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    std::lock_guard<std::mutex> guard(mutex_);
    files_.insert_or_assign(std::move(name), std::move(buffer));
}

std::string ByteBuffersDirectory::new_file_name(std::string_view prefix, std::string_view suffix) {
    const int64_t counter = counter_.fetch_add(1) + 1;
    return std::format("{}_{}{}", prefix, counter, suffix);
}

} // namespace lucene
