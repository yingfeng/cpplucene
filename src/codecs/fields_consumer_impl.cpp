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

module lucene_core:fields_consumer.impl;

import :fields_consumer;

import std;

import :error;
import :fields;
import :fields_producer;
import :mapped_multi_fields;
import :merge_state;
import :multi_fields;
import :norms_producer;

namespace lucene {

VoidResult FieldsConsumer::merge(const SegmentWriteState &state, const SegmentInfo &segment_info,
                                 const MergeState &merge_state, NormsProducer *norms) {
    std::vector<MultiFields::Sub> subs;
    subs.reserve(merge_state.fields_producers.size());

    int32_t doc_base = 0;
    for (size_t i = 0; i < merge_state.fields_producers.size(); ++i) {
        const auto &producer = merge_state.fields_producers[i];
        const int32_t max_doc = merge_state.max_docs[i];
        if (producer != nullptr) {
            if (auto checked = producer->check_integrity(); !checked.has_value()) {
                return checked;
            }
            subs.push_back(MultiFields::Sub{producer.get(), ReaderSlice(doc_base, max_doc, i)});
        }
        doc_base += max_doc;
    }

    auto multi_fields = std::make_unique<MultiFields>(std::move(subs));
    auto meta = std::make_shared<const MergeStateMeta>(merge_state.get_meta());
    MappedMultiFields merged_fields(std::move(meta), std::move(multi_fields));
    return write(state, segment_info, merged_fields, norms);
}

} // namespace lucene
