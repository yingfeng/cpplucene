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

export module lucene_core:fields_consumer;

import :types;
import std;

import :close;
import :error;
import :fields;
import :norms_producer;
import :segment_info;
import :segment_write_state;

namespace lucene {

/// Abstract API that consumes terms, doc, freq, prox, offset and payloads
/// postings.  Concrete implementations do "something" with the postings, such
/// as writing them into the index in a specific format.
///
/// Port of the `FieldsConsumer` trait (`core/codecs/fields_consumer.rs`).
///
/// **Deviation:** rlucene's `merge` has a default implementation built on
/// `MultiFields` / `MappedMultiFields`, and the merge subsystem is not ported
/// yet, so `merge` is absent for now; callers use [`write`] on the already
/// merged `Fields`.
export class FieldsConsumer : public Closeable {
public:
    ~FieldsConsumer() override = default;

    /// Writes all fields, terms and postings.  This is the "pull" API: the
    /// implementation may iterate more than once over the postings.
    ///
    /// Notes (from Lucene):
    ///  * index statistics have to be computed here: each term's `doc_freq` and
    ///    `total_term_freq`, plus the summary `sum_total_term_freq`,
    ///    `sum_total_doc_freq` and `doc_count`;
    ///  * terms without documents and fields without terms must be skipped, even
    ///    though the provided [`Fields`] exposes them, which typically requires
    ///    writing a field or term lazily;
    ///  * the provided [`Fields`] is limited: statistics may not be queried and
    ///    live docs may not be passed when pulling postings.
    ///
    /// `state` and `segment_info` must describe the segment being written.
    [[nodiscard]] virtual VoidResult write(const SegmentWriteState &state, const SegmentInfo &segment_info,
                                           Fields &fields, NormsProducer *norms) = 0;
};

} // namespace lucene
