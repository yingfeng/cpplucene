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

export module lucene_core:vector_util;

import :types;
import std;

namespace lucene {

/// Vectorised helpers over `int32_t` buffers.
///
/// Port of `VectorUtil` (`core/internal/vectorization/`), which the postings
/// decoders use to find the first document of a block that is at or after a
/// target.  The reference implementation dispatches on the compiled-in ISA; this
/// port compiles the widest kernel for the target (`-march` driven) and falls
/// back to the scalar scan everywhere else.
export class VectorUtil {
public:
    VectorUtil() = delete;

    /// The first index in `[from, to)` whose value is at or after `target`, or
    /// `to` when there is none.
    ///
    /// A block holds 128 values and the target is almost always somewhere in the
    /// middle, so a branchy scalar scan spends most of its time on branch
    /// mispredictions; the vector kernel compares eight lanes at a time and
    /// resolves the position from a bit mask.
    [[nodiscard]] static size_t find_next_geq(std::span<const int32_t> buffer, int32_t target, size_t from,
                                             size_t to);
};

} // namespace lucene
