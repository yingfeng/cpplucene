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

export module lucene_core:impacts_source;

import :types;
import std;

import :error;
import :impacts;

namespace lucene {

/// Source of [`Impacts`].  Port of the `ImpactsSource` trait
/// (`core/index/impacts_source.rs`).
///
/// NOTE: advancing the iterator may invalidate the returned impacts, so they
/// must not be used after the iterator has been advanced.
export class ImpactsSource {
public:
    virtual ~ImpactsSource() = default;

    /// Shallow-advance to `target`.
    ///
    /// This is cheaper than `DocIdSetIterator::advance(target)` and allows
    /// later `get_impacts()` calls to ignore doc ids below `target` in order to
    /// report more precise impacts.
    [[nodiscard]] virtual VoidResult advance_shallow(int32_t target) = 0;

    /// Information about upcoming impacts for doc ids greater than or equal to
    /// the max of the current doc id and the last `advance_shallow` target.
    ///
    /// rlucene models the return type as the GAT `Self::Impacts<'a>`; the port
    /// type-erases it to `Impacts` because C++ has no generic associated types.
    [[nodiscard]] virtual Result<std::unique_ptr<Impacts>> get_impacts() const = 0;
};

} // namespace lucene
