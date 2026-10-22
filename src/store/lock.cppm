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

export module lucene_core:lock;

import :types;
import :error;

import std;

namespace lucene {

/// An inter-process lock owned by a [`Directory`].
///
/// Port of the `Lock` trait (`core/store/lock.rs`).  The lock is released by
/// `close()`; destroying the object also releases it.
export class Lock {
public:
    virtual ~Lock() = default;

    /// Best effort check that the lock is still held.
    [[nodiscard]] virtual VoidResult ensure_valid() const = 0;

    /// Releases the lock.  Idempotent.
    [[nodiscard]] virtual VoidResult close() = 0;

    [[nodiscard]] virtual std::string to_string() const = 0;
};

/// A lock that always succeeds and never blocks (`NoLock`).
export class NoLock : public Lock {
public:
    [[nodiscard]] VoidResult ensure_valid() const override { return {}; }
    [[nodiscard]] VoidResult close() override { return {}; }
    [[nodiscard]] std::string to_string() const override { return "NoLock"; }
};

} // namespace lucene
