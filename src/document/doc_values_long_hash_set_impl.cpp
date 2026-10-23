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

module lucene_core:doc_values_long_hash_set.impl;

import :doc_values_long_hash_set;

import std;

import :debug;
import :error;
import :packed_ints;
import :types;

namespace lucene {

DocValuesLongHashSet::DocValuesLongHashSet(std::vector<int64_t> table, size_t mask, bool has_missing_value,
                                           size_t size, int64_t min_value, int64_t max_value)
        : table_(std::move(table)),
          mask_(mask),
          has_missing_value_(has_missing_value),
          size_(size),
          min_value_(min_value),
          max_value_(max_value) {}

Result<DocValuesLongHashSet> DocValuesLongHashSet::create(std::vector<int64_t> values) {
    // The table is a power of two just above 1.5 times the number of values, so
    // that the open addressing stays cheap.
    const int64_t wanted = static_cast<int64_t>(values.size()) * 3 / 2;
    auto bits = PackedInts::bits_required(wanted);
    if (!bits.has_value()) {
        return std::unexpected(std::move(bits.error()));
    }
    const size_t table_size = static_cast<size_t>(1) << bits.value();
    std::vector<int64_t> table(table_size, kDocValuesLongHashSetMissing);
    const size_t mask = table_size - 1;

    bool has_missing_value = false;
    size_t size = 0;
    int64_t previous_value = std::numeric_limits<int64_t>::min();
    for (const int64_t value : values) {
        if (value == kDocValuesLongHashSetMissing) {
            if (!has_missing_value) {
                ++size;
                has_missing_value = true;
            }
        } else if (add(table, mask, value)) {
            ++size;
        }
        debug_check(value >= previous_value, "values must be provided in sorted order");
        previous_value = value;
    }

    const int64_t min_value = values.empty() ? std::numeric_limits<int64_t>::max() : values.front();
    const int64_t max_value = values.empty() ? std::numeric_limits<int64_t>::min() : values.back();

    return DocValuesLongHashSet(std::move(table), mask, has_missing_value, size, min_value, max_value);
}

int32_t DocValuesLongHashSet::hash_code(int64_t value) {
    return static_cast<int32_t>(value ^ static_cast<uint64_t>(value) >> 32);
}

bool DocValuesLongHashSet::add(std::vector<int64_t> &table, size_t mask, int64_t value) {
    debug_check(value != kDocValuesLongHashSetMissing, "the missing value is tracked by a flag");
    size_t slot = static_cast<size_t>(hash_code(value)) & mask;
    for (;;) {
        const int64_t present = table[slot];
        if (present == kDocValuesLongHashSetMissing) {
            table[slot] = value;
            return true;
        }
        if (present == value) {
            return false;
        }
        slot = (slot + 1) & mask;
    }
}

bool DocValuesLongHashSet::contains(int64_t value) const {
    if (value == kDocValuesLongHashSetMissing) {
        return has_missing_value_;
    }
    size_t slot = static_cast<size_t>(hash_code(value)) & mask_;
    for (;;) {
        const int64_t present = table_[slot];
        if (present == kDocValuesLongHashSetMissing) {
            return false;
        }
        if (present == value) {
            return true;
        }
        slot = (slot + 1) & mask_;
    }
}

std::string DocValuesLongHashSet::to_string() const {
    std::string result = "[";
    bool first = true;
    if (has_missing_value_) {
        result += std::to_string(kDocValuesLongHashSetMissing);
        first = false;
    }
    for (const int64_t value : table_) {
        if (value == kDocValuesLongHashSetMissing) {
            continue;
        }
        if (!first) {
            result += ", ";
        }
        result += std::to_string(value);
        first = false;
    }
    result += "]";
    return result;
}

} // namespace lucene
