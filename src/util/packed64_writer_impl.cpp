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

module lucene_core:packed64_writer.impl;

import :packed64_writer;

import :data_output;
import :error;
import :packed_ints;
import :packed_stream;
import :types;

import std;

namespace lucene {

Packed64Writer::Packed64Writer(DataOutput &out, int32_t bits_per_value, size_t value_count)
        : out_(&out), bits_per_value_(bits_per_value), value_count_(value_count) {
    values_.reserve(value_count);
}

VoidResult Packed64Writer::add(int64_t value) {
    if (finished_) {
        return err(LuceneError::illegal_state("this Packed64Writer is finished"));
    }
    if (values_.size() >= value_count_) {
        return err(LuceneError::illegal_state("the value count " + std::to_string(value_count_) +
                                              " is already written"));
    }
    // Lucene checks the value against the width; a negative value never fits.
    if (value < 0 || (bits_per_value_ < 64 && value > PackedInts::max_value(bits_per_value_))) {
        return err(LuceneError::illegal_argument(std::format("value {} does not fit in {} bits", value,
                                                            bits_per_value_)));
    }
    values_.push_back(value);
    return {};
}

VoidResult Packed64Writer::finish() {
    if (finished_) {
        return err(LuceneError::already_closed("this Packed64Writer is finished"));
    }
    if (values_.size() != value_count_) {
        return err(LuceneError::illegal_state("wrote " + std::to_string(values_.size()) + " values, expected " +
                                              std::to_string(value_count_)));
    }
    finished_ = true;
    return write_packed_stream(*out_, values_, bits_per_value_);
}

} // namespace lucene
