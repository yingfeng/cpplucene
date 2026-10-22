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

export module lucene_core:postings_format;

import :types;
import std;

import :error;
import :fields_consumer;
import :fields_producer;
import :segment_info;
import :segment_read_state;
import :segment_write_state;

namespace lucene {

/// Encodes and decodes terms, postings and proximity data.
///
/// Port of the `PostingsFormat` trait (`core/codecs/postings_format.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * the associated `FieldsConsumer` / `FieldsProducer` types collapse into the
///    concrete type-erased interfaces;
///  * the reference implementation also has `for_name`, a global registry of postings formats.  This
///    port has a single postings format, so the lookup is the codec's job (see
///    [`Codec::postings_format`]);
///  * the states are passed as `const&` and the segment info separately, which is
///    how the rest of the port threads them (the reference implementation takes the write state by
///    reference and an extra `SegmentInfo`).
export class PostingsFormat {
public:
    virtual ~PostingsFormat() = default;

    /// Name of the format, as recorded in the codec metadata.
    [[nodiscard]] virtual std::string_view name() const = 0;

    /// A consumer for a new segment.
    [[nodiscard]] virtual Result<std::unique_ptr<FieldsConsumer>> fields_consumer(
            const SegmentWriteState &state, const SegmentInfo &segment_info) const = 0;

    /// A producer for an existing segment.
    ///
    /// **NOTE**: by the time this returns, the implementation must hold open
    /// every file it needs; otherwise the files may be deleted by a concurrent
    /// merge.
    [[nodiscard]] virtual Result<std::unique_ptr<FieldsProducer>> fields_producer(
            const SegmentReadState &state, const SegmentInfo &segment_info) const = 0;
};

} // namespace lucene
