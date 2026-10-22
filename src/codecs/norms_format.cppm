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

export module lucene_core:norms_format;

import :types;
import std;

import :error;
import :norms_consumer;
import :norms_producer;
import :segment_info;
import :segment_read_state;
import :segment_write_state;

namespace lucene {

/// Encodes/decodes per-document score normalization values.
///
/// Port of the `NormsFormat` trait (`core/codecs/norms_format.rs`).
///
/// **Deviation:** rlucene parameterises the two methods with the directory's
/// associated `IndexOutput` / `IndexInput` types and returns associated
/// `NormsConsumer<T>` / `NormsProducer<T>` types.  The port returns
/// type-erased `std::unique_ptr`s; the directory reaches the implementation
/// through `SegmentWriteState` / `SegmentReadState`.
export class NormsFormat {
public:
    virtual ~NormsFormat() = default;

    /// A consumer that writes norms to the index.
    [[nodiscard]] virtual Result<std::unique_ptr<NormsConsumer>> norms_consumer(const SegmentWriteState &state,
                                                                               const SegmentInfo &segment_info) = 0;

    /// A producer that reads norms from the index.
    ///
    /// By the time this returns it **must hold open** every file it will need,
    /// otherwise those files may be deleted before they are accessed.  Required
    /// files may also be deleted *during* the call, in which case an error must
    /// be returned so that segment opening is retried with fresh `segments_N`
    /// data.
    [[nodiscard]] virtual Result<std::unique_ptr<NormsProducer>> norms_producer(const SegmentReadState &state,
                                                                                const SegmentInfo &segment_info) = 0;
};

} // namespace lucene
