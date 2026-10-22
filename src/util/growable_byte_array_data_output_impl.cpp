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

module lucene_core:growable_byte_array_data_output.impl;

import :growable_byte_array_data_output;
import :types;
import :error;
import :data_output;

import std;

namespace lucene {

GrowableByteArrayDataOutput::GrowableByteArrayDataOutput(size_t initial_capacity) {
    bytes_.reserve(initial_capacity);
}

VoidResult GrowableByteArrayDataOutput::write_byte(uint8_t b) {
    bytes_.push_back(b);
    return {};
}

VoidResult GrowableByteArrayDataOutput::write_bytes(std::span<const uint8_t> b, size_t offset, size_t length) {
    if (offset + length > b.size()) {
        return err(LuceneError::illegal_argument("write out of bounds"));
    }
    bytes_.insert(bytes_.end(), b.begin() + static_cast<std::vector<uint8_t>::difference_type>(offset),
                  b.begin() + static_cast<std::vector<uint8_t>::difference_type>(offset + length));
    return {};
}

} // namespace lucene
