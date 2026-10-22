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

export module lucene_core:doc_map;

import :types;
import std;

import :error;

namespace lucene {

/// A map of document ids from an input segment to the merged segment.
///
/// Port of the `DocMap` trait (`core/index/merge_state.rs`).
///
/// Merging rebases every document onto the new segment's id space and drops the
/// deleted ones; a negative result means "this document does not survive the
/// merge".
///
/// **Deviation:** the reference implementation needs `impl DocMap for Rc<T>` plus a two-alternative
/// enum to branch between doc map flavours statically.  This port resolves the
/// call through the virtual, so those wrappers disappear.
export class DocMap {
public:
    virtual ~DocMap() = default;

    /// The mapped doc id, or `-1` when the given document is not mapped.
    [[nodiscard]] virtual Result<int32_t> get(int32_t doc_id) const = 0;
};

} // namespace lucene
