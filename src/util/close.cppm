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

export module lucene_core:close;

import :types;
import std;

import :error;

namespace lucene {

/// An object whose `close` may be invoked explicitly.
///
/// Port of the `Closeable` trait (`core/util/close.rs`).  C++ has no `Drop`, so
/// the destructor stays in charge of releasing resources and `close` only
/// performs the part that has to happen deterministically.
export class Closeable {
public:
    virtual ~Closeable() = default;

    /// Releases the resources owned by this object; the default is a no-op.
    [[nodiscard]] virtual VoidResult close() { return {}; }
};

/// [`Closeable`] for shared resources, whose `close` takes `&self` because the
/// caller only holds a `std::shared_ptr` (the reference implementation's `Arc`).
export class CloseableRef {
public:
    virtual ~CloseableRef() = default;

    /// Releases the resources owned by this object; the default is a no-op.
    [[nodiscard]] virtual VoidResult close() const { return {}; }
};

} // namespace lucene
