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

module lucene_core:pending_soft_deletes.impl;

import :pending_soft_deletes;

import std;

import :bits;
import :doc_id_set_iterator;
import :doc_values;
import :error;
import :fixed_bit_set;
import :leaf_reader;
import :types;

namespace lucene {

Result<PendingSoftDeletes> PendingSoftDeletes::create(std::string field, LeafReader &reader) {
    if (field.empty()) {
        return err(LuceneError::illegal_argument("the soft deletes field must not be empty"));
    }
    auto values = reader.get_numeric_doc_values(field);
    if (!values.has_value()) {
        return std::unexpected(std::move(values.error()));
    }
    std::vector<std::pair<int32_t, int64_t>> generations;
    if (values.value() != nullptr) {
        NumericDocValues &dv = *values.value();
        while (true) {
            auto doc = dv.next_doc();
            if (!doc.has_value()) {
                return std::unexpected(std::move(doc.error()));
            }
            if (doc.value() == NO_MORE_DOCS) {
                break;
            }
            auto value = dv.long_value();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            if (value.value() != 0) {
                generations.emplace_back(doc.value(), value.value());
            }
        }
    }
    return PendingSoftDeletes(std::move(field), std::move(generations));
}

int32_t PendingSoftDeletes::num_soft_deletes_below(int64_t generation) const noexcept {
    int32_t count = 0;
    for (const auto &[doc, value] : generations_) {
        (void)doc;
        if (value <= generation) {
            ++count;
        }
    }
    return count;
}

int64_t PendingSoftDeletes::min_generation() const noexcept {
    int64_t smallest = 0;
    for (const auto &[doc, value] : generations_) {
        (void)doc;
        if (smallest == 0 || value < smallest) {
            smallest = value;
        }
    }
    return smallest;
}

int64_t PendingSoftDeletes::max_generation() const noexcept {
    int64_t largest = 0;
    for (const auto &[doc, value] : generations_) {
        (void)doc;
        if (value > largest) {
            largest = value;
        }
    }
    return largest;
}

std::vector<int32_t> PendingSoftDeletes::doc_ids_below(int64_t generation) const {
    std::vector<int32_t> docs;
    for (const auto &[doc, value] : generations_) {
        if (value <= generation) {
            docs.push_back(doc);
        }
    }
    return docs;
}

int64_t PendingSoftDeletes::generation_of(int32_t doc) const noexcept {
    for (const auto &[value_doc, value] : generations_) {
        if (value_doc == doc) {
            return value;
        }
    }
    return 0;
}

Result<int32_t> PendingSoftDeletes::apply_soft_deletes(NumericDocValues &values, FixedBitSet &live) {
    int32_t new_deletes = 0;
    while (true) {
        auto doc = values.next_doc();
        if (!doc.has_value()) {
            return std::unexpected(std::move(doc.error()));
        }
        if (doc.value() == NO_MORE_DOCS) {
            break;
        }
        auto value = values.long_value();
        if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
        }
        const auto index = static_cast<size_t>(doc.value());
        if (value.value() != 0) {
            if (live.clear(index)) {
                // It was live and is soft deleted now.
                ++new_deletes;
            }
        } else if (live.set(index)) {
            // It was soft deleted and its delete expired: the document is live again.
            --new_deletes;
        }
    }
    return new_deletes;
}

Result<int32_t> PendingSoftDeletes::count_soft_deletes(DocIdSetIterator *soft_deleted, const Bits *hard_deletes) {
    int32_t count = 0;
    if (soft_deleted == nullptr) {
        return count;
    }
    while (true) {
        auto doc = soft_deleted->next_doc();
        if (!doc.has_value()) {
            return std::unexpected(std::move(doc.error()));
        }
        if (doc.value() == NO_MORE_DOCS) {
            break;
        }
        if (hard_deletes != nullptr) {
            auto hard_deleted = hard_deletes->get(static_cast<size_t>(doc.value()));
            if (!hard_deleted.has_value()) {
                return std::unexpected(std::move(hard_deleted.error()));
            }
            if (hard_deleted.value()) {
                continue;
            }
        }
        ++count;
    }
    return count;
}

} // namespace lucene
