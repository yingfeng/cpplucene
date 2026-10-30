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

/// `FstOutput::get` already answers 0 for a slot that holds nothing or another
/// type, which is this algebra's NO_OUTPUT.
int64_t as_int(const FstOutput &value) { return value.get<int64_t>(); }

} // namespace

const FstPositiveIntOutputs &FstPositiveIntOutputs::singleton() {
    static const FstPositiveIntOutputs instance;
    return instance;
}

FstOutput FstPositiveIntOutputs::common(const FstOutput &output1, const FstOutput &output2) const {
    const int64_t first = as_int(output1);
    const int64_t second = as_int(output2);
    if (first == 0 || second == 0) {
        return get_no_output();
    }
    debug_check(first > 0 && second > 0, "PositiveIntOutputs only accepts non-negative values");
    return FstOutput{std::min(first, second)};
}

FstOutput FstPositiveIntOutputs::subtract(const FstOutput &output, const FstOutput &inc) const {
    const int64_t value = as_int(output);
    const int64_t increment = as_int(inc);
    debug_check(value >= increment, "the subtracted output must not be greater than the output");
    if (increment == 0) {
        return FstOutput{value};
    }
    if (value == increment) {
        return get_no_output();
    }
    return FstOutput{value - increment};
}

FstOutput FstPositiveIntOutputs::add(const FstOutput &prefix, const FstOutput &output) const {
    const int64_t first = as_int(prefix);
    const int64_t second = as_int(output);
    if (first == 0) {
        return FstOutput{second};
    }
    if (second == 0) {
        return FstOutput{first};
    }
    return FstOutput{first + second};
}

VoidResult FstPositiveIntOutputs::write(const FstOutput &output, DataOutput &out) const {
    debug_check(as_int(output) >= 0, "PositiveIntOutputs only accepts non-negative values");
    return out.write_vlong(as_int(output));
}

Result<FstOutput> FstPositiveIntOutputs::read(DataInput &input) const {
    auto value = input.read_vlong();
    if (!value.has_value()) {
        return err(value.error());
    }
    if (value.value() == 0) {
        return get_no_output();
    }
    return FstOutput{value.value()};
}

FstOutput FstPositiveIntOutputs::get_no_output() const {
    return FstOutput{int64_t{0}};
}

bool FstPositiveIntOutputs::is_no_output(const FstOutput &output) const {
    return as_int(output) == 0;
}

std::string FstPositiveIntOutputs::output_to_string(const FstOutput &output) const {
    return std::to_string(as_int(output));
}

int64_t FstPositiveIntOutputs::ram_bytes_used(const FstOutput & /*output*/) const {
    return static_cast<int64_t>(sizeof(int64_t));
}

} // namespace lucene
