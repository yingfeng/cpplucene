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

module lucene_core:binary_range_doc_values.impl;

import :binary_range_doc_values;

import std;

import :binary_doc_values;
import :bytes_ref;
import :doc_values_iterator;
import :error;
import :types;

namespace lucene {

BinaryRangeDocValues::BinaryRangeDocValues(std::unique_ptr<BinaryDocValues> in, size_t num_dims,
                                           size_t num_bytes_per_dimension)
        : in_(std::move(in)),
          packed_value_(2 * num_dims * num_bytes_per_dimension, 0),
          num_dims_(num_dims),
          num_bytes_per_dimension_(num_bytes_per_dimension) {}

int32_t BinaryRangeDocValues::doc_id() const {
    return in_->doc_id();
}

Result<int32_t> BinaryRangeDocValues::next_doc() {
    auto doc = in_->next_doc();
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    if (doc.value() != NO_MORE_DOCS) {
        if (auto decoded = decode_ranges(); !decoded.has_value()) {
            return std::unexpected(std::move(decoded.error()));
        }
    }
    return doc;
}

Result<int32_t> BinaryRangeDocValues::advance(int32_t target) {
    auto doc = in_->advance(target);
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    if (doc.value() != NO_MORE_DOCS) {
        if (auto decoded = decode_ranges(); !decoded.has_value()) {
            return std::unexpected(std::move(decoded.error()));
        }
    }
    return doc;
}

Result<int32_t> BinaryRangeDocValues::slow_advance(int32_t target) {
    auto doc = in_->slow_advance(target);
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    if (doc.value() != NO_MORE_DOCS) {
        if (auto decoded = decode_ranges(); !decoded.has_value()) {
            return std::unexpected(std::move(decoded.error()));
        }
    }
    return doc;
}

Result<int64_t> BinaryRangeDocValues::cost() const {
    return in_->cost();
}

Result<bool> BinaryRangeDocValues::advance_exact(int32_t target) {
    auto exact = in_->advance_exact(target);
    if (!exact.has_value()) {
        return std::unexpected(std::move(exact.error()));
    }
    if (exact.value()) {
        if (auto decoded = decode_ranges(); !decoded.has_value()) {
            return std::unexpected(std::move(decoded.error()));
        }
    }
    return exact;
}

Result<BytesRef> BinaryRangeDocValues::binary_value() {
    return in_->binary_value();
}

VoidResult BinaryRangeDocValues::decode_ranges() {
    auto value = in_->binary_value();
    if (!value.has_value()) {
        return std::unexpected(std::move(value.error()));
    }
    const std::span<const uint8_t> source = value.value().as_slice();
    if (source.size() < packed_value_.size()) {
        return err(LuceneError::illegal_argument("the doc values value is too short for " + std::to_string(num_dims_) +
                                                " dimension(s) of " + std::to_string(num_bytes_per_dimension_) +
                                                " bytes"));
    }
    std::copy_n(source.begin(), static_cast<std::ptrdiff_t>(packed_value_.size()), packed_value_.begin());
    return {};
}

} // namespace lucene
