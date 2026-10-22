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

module lucene_core:block_tree_output_accumulator.impl;

import :block_tree_output_accumulator;

import :bytes_ref;
import :data_input;
import :debug;
import :error;
import :types;

import std;

namespace lucene {

void OutputAccumulator::push(const BytesRef &output) {
    if (output.length() == 0) {
        return;
    }
    debug_check(output.length() > 0);
    if (outputs_.size() == num_) {
        outputs_.push_back(output);
    } else if (outputs_[num_] != output) {
        outputs_[num_] = output;
    }
    ++num_;
}

void OutputAccumulator::pop(const BytesRef &output) {
    if (output.length() == 0) {
        return;
    }
    debug_check(num_ > 0);
    debug_check(outputs_[num_ - 1] == output);
    --num_;
}

void OutputAccumulator::pop_n(size_t count) {
    debug_check(num_ >= count);
    num_ -= count;
}

const BytesRef &OutputAccumulator::output_at(size_t position) const {
    debug_check(position < num_);
    return outputs_[position];
}

VoidResult OutputAccumulator::set_floor_data(std::vector<uint8_t> &floor_data) const {
    debug_check(current_index_ + 1 == num_);
    if (current_index_ >= num_) {
        return err(LuceneError::illegal_state("floor data should be stored in the last output"));
    }
    const BytesRef &output = outputs_[current_index_];
    if (index_ > output.length()) {
        return err(LuceneError::illegal_state("floor data cursor is past the end of the output"));
    }
    const std::span<const uint8_t> remaining = output.as_slice().subspan(index_);
    floor_data.assign(remaining.begin(), remaining.end());
    return {};
}

Result<uint8_t> OutputAccumulator::read_byte() {
    if (current_index_ >= num_) {
        return err(LuceneError::illegal_state("OutputAccumulator is exhausted"));
    }
    const BytesRef &current = outputs_[current_index_];
    if (index_ >= current.length()) {
        ++current_index_;
        index_ = 0;
        if (current_index_ >= num_) {
            return err(LuceneError::illegal_state("OutputAccumulator is exhausted"));
        }
    }
    const uint8_t byte = outputs_[current_index_].as_slice()[index_];
    ++index_;
    return byte;
}

} // namespace lucene
