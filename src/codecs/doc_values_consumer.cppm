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

export module lucene_core:doc_values_consumer;

import :types;
import std;

import :close;
import :doc_values_producer;
import :error;
import :field_infos;

namespace lucene {

/// Writes the doc values of a segment.
///
/// Port of the `DocValuesConsumer` trait (`core/codecs/doc_values_consumer.rs`).
///
/// **Deviations:**
///  * rlucene passes the write state and the segment info to every `add_*_field`;
///    the concrete consumer already holds both here, so the methods only need the
///    field and its source, like Lucene's Java API;
///  * `merge` is not part of the interface yet, because merging needs
///    `MergeState`.
export class DocValuesConsumer : public Closeable {
public:
    ~DocValuesConsumer() override = default;

    /// Copies the numeric values of `field`.
    [[nodiscard]] virtual VoidResult add_numeric_field(const FieldInfo &field, DocValuesProducer &producer) = 0;

    /// Copies the binary values of `field`.
    [[nodiscard]] virtual VoidResult add_binary_field(const FieldInfo &field, DocValuesProducer &producer) = 0;

    /// Copies the sorted values of `field`.
    [[nodiscard]] virtual VoidResult add_sorted_field(const FieldInfo &field, DocValuesProducer &producer) = 0;

    /// Copies the sorted numeric values of `field`.
    [[nodiscard]] virtual VoidResult add_sorted_numeric_field(const FieldInfo &field,
                                                              DocValuesProducer &producer) = 0;

    /// Copies the sorted set values of `field`.
    [[nodiscard]] virtual VoidResult add_sorted_set_field(const FieldInfo &field, DocValuesProducer &producer) = 0;
};

} // namespace lucene
