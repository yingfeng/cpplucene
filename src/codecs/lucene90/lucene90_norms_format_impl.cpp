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

module lucene_core:lucene90_norms_format.impl;

import :lucene90_norms_format;

import :error;
import :lucene90_norms_consumer;
import :lucene90_norms_producer;
import :norms_consumer;
import :norms_producer;
import :segment_info;
import :segment_read_state;
import :segment_write_state;
import :types;

import std;

namespace lucene {

Result<std::unique_ptr<NormsConsumer>> Lucene90NormsFormat::norms_consumer(const SegmentWriteState &state,
                                                                          const SegmentInfo &segment_info) {
    auto consumer = Lucene90NormsConsumer::create(state, segment_info);
    if (!consumer.has_value()) {
        return std::unexpected(std::move(consumer.error()));
    }
    return std::unique_ptr<NormsConsumer>(std::move(consumer.value()));
}

Result<std::unique_ptr<NormsProducer>> Lucene90NormsFormat::norms_producer(const SegmentReadState &state,
                                                                          const SegmentInfo &segment_info) {
    auto producer = Lucene90NormsProducer::create(state, segment_info);
    if (!producer.has_value()) {
        return std::unexpected(std::move(producer.error()));
    }
    return std::unique_ptr<NormsProducer>(std::move(producer.value()));
}

} // namespace lucene
