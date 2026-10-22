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

export module lucene_core:doc_values_writer;

import :types;
import std;

import :doc_values_consumer;
import :error;

namespace lucene {

/// Buffers the doc values of one field until the segment is flushed.
///
/// Port of the `DocValuesWriter` trait (`index/doc_values_writer.rs`).
///
/// **Deviations** (no index sorting in this port):
///  * `flush` takes only the consumer: the segment write state and segment info
///    are already held by the concrete consumer, and `sort_map` does not exist
///    because `IndexWriterConfig.index_sort` is not ported;
///  * the trait's `get_doc_values` (used by the in-memory update path) is not
///    part of the interface; the two buffered writers expose their own
///    accessors instead.
export class DocValuesWriter {
public:
    virtual ~DocValuesWriter() = default;

    /// Freezes the buffered values; must run before [`flush`].
    [[nodiscard]] virtual VoidResult finish() = 0;

    /// Writes the buffered values into the segment through `consumer`.
    [[nodiscard]] virtual VoidResult flush(DocValuesConsumer &consumer) = 0;
};

} // namespace lucene
