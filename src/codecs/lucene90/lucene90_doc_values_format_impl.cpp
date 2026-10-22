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

module lucene_core:lucene90_doc_values_format.impl;

import :lucene90_doc_values_format;

import :doc_values_consumer;
import :doc_values_producer;
import :error;
import :lucene90_doc_values_consumer;
import :lucene90_doc_values_producer;
import :segment_info;
import :segment_read_state;
import :segment_write_state;

import std;

namespace lucene {

Result<std::unique_ptr<DocValuesConsumer>> Lucene90DocValuesFormat::fields_consumer(
        const SegmentWriteState &state, const SegmentInfo &segment_info) {
    return Lucene90DocValuesConsumer::create(state, segment_info);
}

Result<std::unique_ptr<DocValuesProducer>> Lucene90DocValuesFormat::fields_producer(
        const SegmentReadState &state, const SegmentInfo &segment_info) {
    return Lucene90DocValuesProducer::create(state, segment_info, DATA_CODEC, DATA_EXTENSION, META_CODEC,
                                             META_EXTENSION);
}

} // namespace lucene
