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

export module lucene_core:lock_factory;

import :types;
import :error;

import std;

import :lock;

namespace lucene {

/// Creates the locks used by a [`Directory`].  Port of the `LockFactory` trait.
///
/// rlucene passes the `Directory` to `obtain_lock`; here the concrete factories
/// capture what they need (a filesystem path) at construction time, which keeps
/// `Directory` and `LockFactory` free of a circular dependency.
export class LockFactory {
public:
    virtual ~LockFactory() = default;

    /// Obtains the lock called `lock_name`, failing if it is already held.
    [[nodiscard]] virtual Result<std::unique_ptr<Lock>> obtain_lock(std::string_view lock_name) = 0;

    /// Clears (deletes) the lock called `lock_name`.
    [[nodiscard]] virtual VoidResult clear_lock(std::string_view lock_name) = 0;
};

/// Returns [`NoLock`] instances: no locking at all.
export class NoLockFactory : public LockFactory {
public:
    [[nodiscard]] Result<std::unique_ptr<Lock>> obtain_lock(std::string_view /*lock_name*/) override;
    [[nodiscard]] VoidResult clear_lock(std::string_view /*lock_name*/) override { return {}; }
};

/// A lock factory whose locks are only visible inside this process.
///
/// This is what `ByteBuffersDirectory` uses, matching `SingleInstanceLockFactory`.
export class SingleInstanceLockFactory : public LockFactory {
public:
    [[nodiscard]] Result<std::unique_ptr<Lock>> obtain_lock(std::string_view lock_name) override;
    [[nodiscard]] VoidResult clear_lock(std::string_view lock_name) override;

private:
    std::mutex mutex_;
    std::set<std::string, std::less<>> held_;
};

/// An advisory POSIX lock on `<path>/<lock_name>.lck`, using `fcntl(F_SETLK)`.
export class NativeFSLockFactory : public LockFactory {
public:
    explicit NativeFSLockFactory(std::filesystem::path lock_dir);

    [[nodiscard]] Result<std::unique_ptr<Lock>> obtain_lock(std::string_view lock_name) override;
    [[nodiscard]] VoidResult clear_lock(std::string_view lock_name) override;

    [[nodiscard]] const std::filesystem::path &lock_dir() const noexcept { return lock_dir_; }

private:
    [[nodiscard]] std::filesystem::path lock_file(std::string_view lock_name) const;

    std::filesystem::path lock_dir_;
};

} // namespace lucene
