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

module lucene_core:fst_positive_int_outputs.impl;

import :fst_positive_int_outputs;
import :fst_outputs;
import :types;
import :error;
import :debug;
import :data_input;
import :data_output;

import std;

namespace lucene {

namespace {

int64_t as_int(const std::any &value) {
    if (value.type() != typeid(int64_t)) {
        return 0;
    }
    return std::any_cast<int64_t>(value);
}

} // namespace

const FstPositiveIntOutputs &FstPositiveIntOutputs::singleton() {
    static const FstPositiveIntOutputs instance;
    return instance;
}

std::any FstPositiveIntOutputs::common(const std::any &output1, const std::any &output2) const {
    const int64_t first = as_int(output1);
    const int64_t second = as_int(output2);
    if (first == 0 || second == 0) {
        return get_no_output();
    }
    debug_check(first > 0 && second > 0, "PositiveIntOutputs only accepts non-negative values");
    return std::any{std::min(first, second)};
}

std::any FstPositiveIntOutputs::subtract(const std::any &output, const std::any &inc) const {
    const int64_t value = as_int(output);
    const int64_t increment = as_int(inc);
    debug_check(value >= increment, "the subtracted output must not be greater than the output");
    if (increment == 0) {
        return std::any{value};
    }
    if (value == increment) {
        return get_no_output();
    }
    return std::any{value - increment};
}

std::any FstPositiveIntOutputs::add(const std::any &prefix, const std::any &output) const {
    const int64_t first = as_int(prefix);
    const int64_t second = as_int(output);
    if (first == 0) {
        return std::any{second};
    }
    if (second == 0) {
        return std::any{first};
    }
    return std::any{first + second};
}

VoidResult FstPositiveIntOutputs::write(const std::any &output, DataOutput &out) const {
    debug_check(as_int(output) >= 0, "PositiveIntOutputs only accepts non-negative values");
    return out.write_vlong(as_int(output));
}

Result<std::any> FstPositiveIntOutputs::read(DataInput &input) const {
    auto value = input.read_vlong();
    if (!value.has_value()) {
        return err(value.error());
    }
    if (value.value() == 0) {
        return get_no_output();
    }
    return std::any{value.value()};
}

std::any FstPositiveIntOutputs::get_no_output() const {
    return std::any{int64_t{0}};
}

bool FstPositiveIntOutputs::is_no_output(const std::any &output) const {
    return as_int(output) == 0;
}

std::string FstPositiveIntOutputs::output_to_string(const std::any &output) const {
    return std::to_string(as_int(output));
}

int64_t FstPositiveIntOutputs::ram_bytes_used(const std::any & /*output*/) const {
    return static_cast<int64_t>(sizeof(int64_t));
}

} // namespace lucene
