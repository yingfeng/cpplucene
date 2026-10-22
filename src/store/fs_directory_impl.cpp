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
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

module lucene_core:fs_directory.impl;

import :fs_directory;
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

constexpr size_t WRITE_BUFFER_SIZE = 64 * 1024;

/// Owns a file descriptor and closes it exactly once.
class FdOwner {
public:
    explicit FdOwner(int fd) : fd_(fd) {}
    FdOwner(const FdOwner &) = delete;
    FdOwner &operator=(const FdOwner &) = delete;
    ~FdOwner() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    [[nodiscard]] int get() const noexcept { return fd_; }

private:
    int fd_;
};

using FdPtr = std::shared_ptr<FdOwner>;

[[nodiscard]] Result<size_t> file_size(int fd) {
    struct stat info {};
    if (::fstat(fd, &info) != 0) {
        return err(LuceneError::io(std::format("fstat failed: {}", std::strerror(errno))));
    }
    return static_cast<size_t>(info.st_size);
}

/// Reads exactly `bytes.size()` bytes at `position`.
[[nodiscard]] VoidResult pread_exact(int fd, size_t position, std::span<uint8_t> bytes) {
    size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t read = ::pread(fd,
                                     bytes.data() + done,
                                     bytes.size() - done,
                                     static_cast<off_t>(position + done));
        if (read < 0) {
            if (errno == EINTR) {
                continue;
            }
            return err(LuceneError::io(std::format("pread failed: {}", std::strerror(errno))));
        }
        if (read == 0) {
            return err(LuceneError::eof(
                std::format("read past eof: position={} requested={} got={}", position, bytes.size(), done)));
        }
        done += static_cast<size_t>(read);
    }
    return {};
}

[[nodiscard]] VoidResult pwrite_all(int fd, std::span<const uint8_t> bytes, size_t position) {
    size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t written = ::pwrite(fd,
                                         bytes.data() + done,
                                         bytes.size() - done,
                                         static_cast<off_t>(position + done));
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return err(LuceneError::io(std::format("pwrite failed: {}", std::strerror(errno))));
        }
        done += static_cast<size_t>(written);
    }
    return {};
}

/// Random access through `pread`, no per-object state.
class FSRandomAccessInput : public RandomAccessInput {
public:
    FSRandomAccessInput(FdPtr fd, size_t offset, size_t length)
        : fd_(std::move(fd)), offset_(offset), length_(length) {}

    [[nodiscard]] Result<uint8_t> read_byte(size_t pos) const override {
        if (pos >= length_) {
            return err(LuceneError::eof(std::format("read past slice end: pos={} length={}", pos, length_)));
        }
        uint8_t byte = 0;
        if (auto r = pread_exact(fd_->get(), offset_ + pos, std::span<uint8_t>(&byte, 1)); !r) {
            return std::unexpected(std::move(r.error()));
        }
        return byte;
    }

    [[nodiscard]] Result<int16_t> read_short(size_t pos) const override {
        std::array<uint8_t, 2> buffer{};
        if (pos + buffer.size() > length_) {
            return err(LuceneError::eof(std::format("read past slice end: pos={} length={}", pos, length_)));
        }
        if (auto r = pread_exact(fd_->get(), offset_ + pos, buffer); !r) {
            return std::unexpected(std::move(r.error()));
        }
        return BitUtil::get_i16_le(buffer, 0);
    }

    [[nodiscard]] Result<int32_t> read_int(size_t pos) const override {
        std::array<uint8_t, 4> buffer{};
        if (pos + buffer.size() > length_) {
            return err(LuceneError::eof(std::format("read past slice end: pos={} length={}", pos, length_)));
        }
        if (auto r = pread_exact(fd_->get(), offset_ + pos, buffer); !r) {
            return std::unexpected(std::move(r.error()));
        }
        return BitUtil::get_i32_le(buffer, 0);
    }

    [[nodiscard]] Result<int64_t> read_long(size_t pos) const override {
        std::array<uint8_t, 8> buffer{};
        if (pos + buffer.size() > length_) {
            return err(LuceneError::eof(std::format("read past slice end: pos={} length={}", pos, length_)));
        }
        if (auto r = pread_exact(fd_->get(), offset_ + pos, buffer); !r) {
            return std::unexpected(std::move(r.error()));
        }
        return BitUtil::get_i64_le(buffer, 0);
    }

    [[nodiscard]] VoidResult read_bytes(size_t pos, std::span<uint8_t> bytes) const override {
        if (pos + bytes.size() > length_) {
            return err(LuceneError::eof(
                std::format("read past slice end: pos={} requested={} length={}", pos, bytes.size(), length_)));
        }
        return pread_exact(fd_->get(), offset_ + pos, bytes);
    }

    [[nodiscard]] Result<size_t> length() const override { return length_; }

private:
    FdPtr fd_;
    size_t offset_{0};
    size_t length_{0};
};

/// A positional-read [`IndexInput`] over a file region.
class FSIndexInput : public IndexInput {
public:
    FSIndexInput(FdPtr fd, size_t offset, size_t length, std::string description)
        : fd_(std::move(fd)), offset_(offset), length_(length) {
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
        uint8_t byte = 0;
        if (auto r = pread_exact(fd_->get(), offset_ + pos_, std::span<uint8_t>(&byte, 1)); !r) {
            return std::unexpected(std::move(r.error()));
        }
        ++pos_;
        return byte;
    }

    [[nodiscard]] VoidResult read_bytes(std::span<uint8_t> b) override {
        if (pos_ + b.size() > length_) {
            return err(LuceneError::eof(
                std::format("read past end: pos={} requested={} length={}", pos_, b.size(), length_)));
        }
        if (auto r = pread_exact(fd_->get(), offset_ + pos_, b); !r) {
            return r;
        }
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
        std::array<uint8_t, 2> buffer{};
        if (pos_ + buffer.size() > length_) {
            return err(LuceneError::eof(std::format("read past end: pos={} length={}", pos_, length_)));
        }
        if (auto r = pread_exact(fd_->get(), offset_ + pos_, buffer); !r) {
            return std::unexpected(std::move(r.error()));
        }
        pos_ += buffer.size();
        return BitUtil::get_i16_le(buffer, 0);
    }

    [[nodiscard]] Result<int32_t> read_int() override {
        std::array<uint8_t, 4> buffer{};
        if (pos_ + buffer.size() > length_) {
            return err(LuceneError::eof(std::format("read past end: pos={} length={}", pos_, length_)));
        }
        if (auto r = pread_exact(fd_->get(), offset_ + pos_, buffer); !r) {
            return std::unexpected(std::move(r.error()));
        }
        pos_ += buffer.size();
        return BitUtil::get_i32_le(buffer, 0);
    }

    [[nodiscard]] Result<int64_t> read_long() override {
        std::array<uint8_t, 8> buffer{};
        if (pos_ + buffer.size() > length_) {
            return err(LuceneError::eof(std::format("read past end: pos={} length={}", pos_, length_)));
        }
        if (auto r = pread_exact(fd_->get(), offset_ + pos_, buffer); !r) {
            return std::unexpected(std::move(r.error()));
        }
        pos_ += buffer.size();
        return BitUtil::get_i64_le(buffer, 0);
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
        return std::unique_ptr<IndexInput>(std::make_unique<FSIndexInput>(
            fd_, offset_ + offset, length, std::string(slice_description)));
    }

    [[nodiscard]] Result<std::unique_ptr<RandomAccessInput>> random_access_slice(size_t offset,
                                                                                size_t length) const override {
        if (offset + length > length_) {
            return err(LuceneError::illegal_argument(std::format("slice [{}, {}) is out of bounds of length {}",
                                                                 offset,
                                                                 offset + length,
                                                                 length_)));
        }
        return std::unique_ptr<RandomAccessInput>(std::make_unique<FSRandomAccessInput>(
            fd_, offset_ + offset, length));
    }

    [[nodiscard]] Result<std::unique_ptr<IndexInput>> clone() const override {
        auto copy = std::make_unique<FSIndexInput>(fd_, offset_, length_, description());
        copy->pos_ = pos_;
        return std::unique_ptr<IndexInput>(std::move(copy));
    }

private:
    FdPtr fd_;
    size_t offset_{0};
    size_t length_{0};
    size_t pos_{0};
};

/// A buffered sequential [`IndexOutput`] over an open file descriptor.
class FSIndexOutput : public IndexOutput {
public:
    FSIndexOutput(int fd, std::string name) : fd_(fd), name_(std::move(name)), buffer_(WRITE_BUFFER_SIZE) {}

    ~FSIndexOutput() override {
        if (!closed_) {
            std::ignore = close();
        }
    }

    [[nodiscard]] Result<size_t> get_file_pointer() const override { return position_ + buffered_; }
    [[nodiscard]] uint64_t get_checksum() override { return crc_.get_value(); }
    [[nodiscard]] std::string_view get_name() const override { return name_; }

    [[nodiscard]] VoidResult write_byte(uint8_t b) override {
        if (buffered_ == buffer_.size()) {
            if (auto r = flush(); !r) {
                return r;
            }
        }
        buffer_[buffered_++] = b;
        crc_.update(b);
        return {};
    }

    [[nodiscard]] VoidResult write_bytes(std::span<const uint8_t> b, size_t offset, size_t length) override {
        if (auto check = CoreHelper::check_from_index_size(offset, length, b.size()); !check) {
            return std::unexpected(std::move(check.error()));
        }
        const auto source = b.subspan(offset, length);
        crc_.update(source);

        // Fill the current buffer, then write whole chunks directly.
        size_t consumed = 0;
        while (consumed < source.size()) {
            if (buffered_ == 0 && source.size() - consumed >= buffer_.size()) {
                const size_t chunk = ((source.size() - consumed) / buffer_.size()) * buffer_.size();
                if (auto r = pwrite_all(fd_, source.subspan(consumed, chunk), position_); !r) {
                    return r;
                }
                position_ += chunk;
                consumed += chunk;
                continue;
            }
            const size_t space = buffer_.size() - buffered_;
            const size_t take = std::min(space, source.size() - consumed);
            std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(consumed), take, buffer_.begin() + buffered_);
            buffered_ += take;
            consumed += take;
            if (buffered_ == buffer_.size()) {
                if (auto r = flush(); !r) {
                    return r;
                }
            }
        }
        return {};
    }

    [[nodiscard]] VoidResult close() override {
        if (closed_) {
            return {};
        }
        closed_ = true;
        if (auto r = flush(); !r) {
            return r;
        }
        if (::close(fd_) != 0) {
            return err(LuceneError::io(std::format("close failed: {}", std::strerror(errno))));
        }
        fd_ = -1;
        return {};
    }

private:
    [[nodiscard]] VoidResult flush() {
        if (buffered_ == 0) {
            return {};
        }
        if (auto r = pwrite_all(fd_, std::span<const uint8_t>(buffer_.data(), buffered_), position_); !r) {
            return r;
        }
        position_ += buffered_;
        buffered_ = 0;
        return {};
    }

    int fd_;
    std::string name_;
    std::vector<uint8_t> buffer_;
    size_t buffered_{0};
    size_t position_{0};
    Crc32 crc_;
    bool closed_{false};
};

} // namespace

// ---------------- FSDirectory ----------------

FSDirectory::FSDirectory(std::filesystem::path path) : path_(std::move(path)) {
    lock_factory_ = std::make_unique<NativeFSLockFactory>(path_);
}

Result<std::unique_ptr<FSDirectory>> FSDirectory::open(const std::filesystem::path &path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        return err(LuceneError::io(
            std::format("cannot create directory {}: {}", path.string(), ec.message())));
    }
    std::unique_ptr<FSDirectory> directory(new FSDirectory(path));
    return directory;
}

std::filesystem::path FSDirectory::file_path(std::string_view name) const { return path_ / std::string(name); }

Result<std::vector<std::string>> FSDirectory::list_all() const {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(path_, ec)) {
        if (entry.is_regular_file()) {
            names.push_back(entry.path().filename().string());
        }
    }
    if (ec) {
        return err(LuceneError::io(std::format("cannot list {}: {}", path_.string(), ec.message())));
    }
    std::ranges::sort(names);
    return names;
}

VoidResult FSDirectory::delete_file(std::string_view name) {
    std::error_code ec;
    const auto path = file_path(name);
    std::filesystem::remove(path, ec);
    if (ec) {
        return err(LuceneError::io(std::format("cannot delete {}: {}", path.string(), ec.message())));
    }
    if (!std::filesystem::exists(path)) {
        return {};
    }
    return err(LuceneError::not_such_file(std::format("file {} does not exist", name)));
}

Result<size_t> FSDirectory::file_length(std::string_view name) const {
    std::error_code ec;
    const auto size = std::filesystem::file_size(file_path(name), ec);
    if (ec) {
        return err(LuceneError::not_such_file(std::format("cannot stat {}: {}", name, ec.message())));
    }
    return static_cast<size_t>(size);
}

Result<std::unique_ptr<IndexOutput>> FSDirectory::create_output(std::string_view name,
                                                               const IOContext & /*context*/) {
    const auto path = file_path(name);
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) {
        return err(LuceneError::io(std::format("cannot create {}: {}", path.string(), std::strerror(errno))));
    }
    return std::unique_ptr<IndexOutput>(std::make_unique<FSIndexOutput>(fd, std::string(name)));
}

Result<CreatedOutput> FSDirectory::create_temp_output(std::string_view prefix,
                                                      std::string_view suffix,
                                                      const IOContext &context) {
    auto name = new_file_name(prefix, suffix);
    auto output = create_output(name, context);
    if (!output) {
        return std::unexpected(std::move(output.error()));
    }
    return CreatedOutput{std::move(name), std::move(output.value())};
}

VoidResult FSDirectory::sync(std::span<const std::string> names) {
    for (const auto &name : names) {
        const auto path = file_path(name);
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return err(LuceneError::io(std::format("cannot open {} for fsync: {}", path.string(), std::strerror(errno))));
        }
        if (::fsync(fd) != 0) {
            const int saved = errno;
            ::close(fd);
            return err(LuceneError::io(std::format("fsync {} failed: {}", path.string(), std::strerror(saved))));
        }
        ::close(fd);
    }
    return {};
}

VoidResult FSDirectory::sync_metadata() {
    const int fd = ::open(path_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return err(LuceneError::io(std::format("cannot open directory for fsync: {}", std::strerror(errno))));
    }
    if (::fsync(fd) != 0) {
        const int saved = errno;
        ::close(fd);
        return err(LuceneError::io(std::format("fsync directory failed: {}", std::strerror(saved))));
    }
    ::close(fd);
    return {};
}

VoidResult FSDirectory::rename(std::string_view source, std::string_view dest) {
    std::error_code ec;
    std::filesystem::rename(file_path(source), file_path(dest), ec);
    if (ec) {
        return err(LuceneError::io(std::format("cannot rename {} to {}: {}", source, dest, ec.message())));
    }
    return {};
}

Result<std::unique_ptr<IndexInput>> FSDirectory::open_input(std::string_view name,
                                                            const IOContext & /*context*/) const {
    const auto path = file_path(name);
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return err(LuceneError::not_such_file(std::format("cannot open {}: {}", path.string(), std::strerror(errno))));
    }
    auto size = file_size(fd);
    if (!size) {
        ::close(fd);
        return std::unexpected(std::move(size.error()));
    }
    FdPtr owner = std::make_shared<FdOwner>(fd);
    return std::unique_ptr<IndexInput>(
        std::make_unique<FSIndexInput>(std::move(owner), 0, size.value(), std::string(name)));
}

Result<std::unique_ptr<Lock>> FSDirectory::obtain_lock(std::string_view name) {
    return lock_factory_->obtain_lock(name);
}

std::string FSDirectory::new_file_name(std::string_view prefix, std::string_view suffix) {
    const int64_t counter = counter_.fetch_add(1) + 1;
    return std::format("{}_{}{}", prefix, counter, suffix);
}

} // namespace lucene
