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

module lucene_core:bkd_point_writer.impl;

import :bkd_point_writer;

import :bkd_heap_point_writer;
import :bkd_offline_point;
import :error;
import :types;

import std;

namespace lucene {

VoidResult PointWriters::append_bytes(PointWriterVariant writer, std::span<const uint8_t> packed_value,
                                      int32_t doc_id) {
    return std::visit(
        [&](auto *typed) -> VoidResult {
            if (typed == nullptr) {
                return err(LuceneError::illegal_state("point writer is null"));
            }
            return typed->append_bytes(packed_value, doc_id);
        },
        writer);
}

VoidResult PointWriters::append_packed(PointWriterVariant writer, std::span<const uint8_t> record) {
    return std::visit(
        [&](auto *typed) -> VoidResult {
            if (typed == nullptr) {
                return err(LuceneError::illegal_state("point writer is null"));
            }
            return typed->append_packed(record);
        },
        writer);
}

size_t PointWriters::count(const PointWriterVariant &writer) {
    return std::visit(
        [](const auto *typed) -> size_t {
            if (typed == nullptr) {
                return 0;
            }
            return typed->count();
        },
        writer);
}

bool PointWriters::is_on_heap(const PointWriterVariant &writer) {
    return std::holds_alternative<HeapPointWriter *>(writer) &&
           std::get<HeapPointWriter *>(writer) != nullptr;
}

HeapPointWriter *PointWriters::heap_writer(const PointWriterVariant &writer) {
    return std::holds_alternative<HeapPointWriter *>(writer) ? std::get<HeapPointWriter *>(writer) : nullptr;
}

OfflinePointWriter *PointWriters::offline_writer(const PointWriterVariant &writer) {
    return std::holds_alternative<OfflinePointWriter *>(writer) ? std::get<OfflinePointWriter *>(writer) : nullptr;
}

} // namespace lucene
