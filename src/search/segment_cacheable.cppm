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

export module lucene_core:segment_cacheable;

import :types;
import std;

import :error;
import :leaf_reader_context;

namespace lucene {

/// Whether an object may be cached against a leaf reader.
///
/// Port of the `SegmentCacheable` trait (`core/search/segment_cacheable.rs`).
///
/// Objects that only depend on segment-immutable structures (points, postings
/// lists) answer `true`; objects that depend on doc values must ask the doc
/// values (`DocValues::is_cacheable`, not ported yet); objects that depend on
/// global statistics or scores answer `false`.
export class SegmentCacheable {
public:
    virtual ~SegmentCacheable() = default;

    [[nodiscard]] virtual Result<bool> is_cacheable(const LeafReaderContext &context) const = 0;
};

} // namespace lucene
