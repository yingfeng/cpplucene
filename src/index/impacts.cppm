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

export module lucene_core:impacts;

import :types;
import std;

import :error;
import :impact;

namespace lucene {

/// Information about upcoming impacts, i.e. `(freq, norm)` pairs.
/// Port of the `Impacts` trait (`core/index/impacts.rs`).
export class Impacts {
public:
    virtual ~Impacts() = default;

    /// Number of levels on which impacts are available.  Always greater than 0
    /// and not necessarily constant, even on a single postings list.
    [[nodiscard]] virtual size_t num_levels() const = 0;

    /// Maximum inclusive doc id until which the impacts returned by
    /// [`get_impacts(level)`] are valid.  Non-decreasing in `level`.
    [[nodiscard]] virtual int32_t get_doc_id_upto(size_t level) const = 0;

    /// Impacts on the given level, sorted by increasing frequency and
    /// increasing unsigned norm.
    ///
    /// NOTE: there is no guarantee that these impacts actually appear in the
    /// postings, only that they trigger scores greater than or equal to the
    /// impacts that do appear.
    [[nodiscard]] virtual Result<std::vector<Impact>> get_impacts(size_t level) const = 0;
};

} // namespace lucene
