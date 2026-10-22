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

export module lucene_core:doc_values_format;

import :types;
import std;

import :doc_values_consumer;
import :doc_values_producer;
import :error;
import :segment_info;
import :segment_read_state;
import :segment_write_state;

namespace lucene {

/// Encodes and decodes per-document values.
///
/// Port of the `DocValuesFormat` trait (`core/codecs/doc_values_format.rs`).
///
/// A producer opened here must hold every file it needs open by the time it
/// returns: a segment file may be deleted as soon as the reader lets it go.
export class DocValuesFormat {
public:
    virtual ~DocValuesFormat() = default;

    /// Name of this format, as stored in the codec's metadata.
    [[nodiscard]] virtual std::string_view name() const = 0;

    /// A consumer that writes the doc values of `state`.
    [[nodiscard]] virtual Result<std::unique_ptr<DocValuesConsumer>> fields_consumer(const SegmentWriteState &state,
                                                                                     const SegmentInfo &segment_info) const = 0;

    /// A producer that reads the doc values of `state`.
    [[nodiscard]] virtual Result<std::unique_ptr<DocValuesProducer>> fields_producer(const SegmentReadState &state,
                                                                                    const SegmentInfo &segment_info) const = 0;
};

} // namespace lucene
