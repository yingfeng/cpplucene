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
#include <unistd.h>

module lucene_core:lock_factory.impl;

import :lock_factory;
import :types;
import :error;
import :lock;

import std;

namespace lucene {

namespace {

/// An in-process lock backed by `SingleInstanceLockFactory`'s bookkeeping.
class SingleInstanceLock : public Lock {
public:
    SingleInstanceLock(SingleInstanceLockFactory *factory, std::string lock_name)
        : factory_(factory), lock_name_(std::move(lock_name)) {}

    [[nodiscard]] VoidResult ensure_valid() const override {
        if (closed_) {
            return err(LuceneError::lock_obtain_failed(std::format("lock {} was already released", lock_name_)));
        }
        return {};
    }

    [[nodiscard]] VoidResult close() override;

    [[nodiscard]] std::string to_string() const override { return std::format("SingleInstanceLock({})", lock_name_); }

private:
    SingleInstanceLockFactory *factory_;
    std::string lock_name_;
    bool closed_{false};
};

/// An advisory lock held on an open file descriptor.
class NativeFSLock : public Lock {
public:
    NativeFSLock(int fd, std::filesystem::path file) : fd_(fd), file_(std::move(file)) {}
    ~NativeFSLock() override { std::ignore = close(); }

    [[nodiscard]] VoidResult ensure_valid() const override;
    [[nodiscard]] VoidResult close() override;
    [[nodiscard]] std::string to_string() const override { return std::format("NativeFSLock({})", file_.string()); }

private:
    int fd_;
    std::filesystem::path file_;
};

VoidResult SingleInstanceLock::close() {
    if (closed_) {
        return {};
    }
    closed_ = true;
    if (factory_ != nullptr) {
        return factory_->clear_lock(lock_name_);
    }
    return {};
}

VoidResult NativeFSLock::ensure_valid() const {
    if (fd_ < 0) {
        return err(LuceneError::lock_obtain_failed("lock is closed"));
    }
    struct flock probe {};
    probe.l_type = F_WRLCK;
    probe.l_whence = SEEK_SET;
    probe.l_start = 0;
    probe.l_len = 0;
    if (::fcntl(fd_, F_GETLK, &probe) != 0) {
        return err(LuceneError::io(std::format("fcntl(F_GETLK) failed: {}", std::strerror(errno))));
    }
    if (probe.l_type != F_UNLCK && probe.l_pid != ::getpid()) {
        return err(LuceneError::lock_obtain_failed(
            std::format("lock {} is held by pid {}", file_.string(), probe.l_pid)));
    }
    return {};
}

VoidResult NativeFSLock::close() {
    if (fd_ < 0) {
        return {};
    }
    ::close(fd_);
    fd_ = -1;
    return {};
}

} // namespace

// ---------------- NoLockFactory ----------------

Result<std::unique_ptr<Lock>> NoLockFactory::obtain_lock(std::string_view /*lock_name*/) {
    return std::unique_ptr<Lock>(std::make_unique<NoLock>());
}

// ---------------- SingleInstanceLockFactory ----------------

Result<std::unique_ptr<Lock>> SingleInstanceLockFactory::obtain_lock(std::string_view lock_name) {
    std::string name(lock_name);
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (held_.contains(name)) {
            return err(LuceneError::lock_obtain_failed(std::format("Lock instance already obtained: {}", name)));
        }
        held_.insert(name);
    }
    return std::unique_ptr<Lock>(std::make_unique<SingleInstanceLock>(this, std::move(name)));
}

VoidResult SingleInstanceLockFactory::clear_lock(std::string_view lock_name) {
    std::lock_guard<std::mutex> guard(mutex_);
    held_.erase(std::string(lock_name));
    return {};
}

// ---------------- NativeFSLockFactory ----------------

NativeFSLockFactory::NativeFSLockFactory(std::filesystem::path lock_dir) : lock_dir_(std::move(lock_dir)) {}

std::filesystem::path NativeFSLockFactory::lock_file(std::string_view lock_name) const {
    return lock_dir_ / std::format("{}.lck", lock_name);
}

Result<std::unique_ptr<Lock>> NativeFSLockFactory::obtain_lock(std::string_view lock_name) {
    const auto path = lock_file(lock_name);
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    if (fd < 0) {
        return err(LuceneError::io(std::format("cannot open lock file {}: {}", path.string(), std::strerror(errno))));
    }
    struct flock lock {};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;
    if (::fcntl(fd, F_SETLK, &lock) != 0) {
        const int saved = errno;
        ::close(fd);
        return err(LuceneError::lock_obtain_failed(
            std::format("cannot obtain lock {}: {}", path.string(), std::strerror(saved))));
    }
    // Keep the file so other processes can see the lock; Lucene removes it when
    // the lock is released normally.
    return std::unique_ptr<Lock>(std::make_unique<NativeFSLock>(fd, path));
}

VoidResult NativeFSLockFactory::clear_lock(std::string_view lock_name) {
    std::error_code ec;
    std::filesystem::remove(lock_file(lock_name), ec);
    if (ec) {
        return err(LuceneError::io(std::format("cannot clear lock: {}", ec.message())));
    }
    return {};
}

} // namespace lucene
