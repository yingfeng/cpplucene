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

module lucene_core:packed64_reader.impl;

import :packed64_reader;

import :error;
import :packed_ints;
import :types;

import std;

namespace lucene {

Packed64Reader::Packed64Reader(std::vector<int64_t> values, int32_t bits_per_value)
        : values_(std::move(values)), bits_per_value_(bits_per_value) {}

int64_t Packed64Reader::get(size_t index) const {
    if (index >= values_.size()) {
        // The reference implementation indexes the backing words without a check;
        // report the mistake instead of reading past the end.
        return std::numeric_limits<int64_t>::min();
    }
    return values_[index];
}

} // namespace lucene
