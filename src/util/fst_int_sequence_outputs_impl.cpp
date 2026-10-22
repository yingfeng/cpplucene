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

module lucene_core:fst_int_sequence_outputs.impl;

import :fst_int_sequence_outputs;
import :fst_outputs;
import :ints_ref;
import :types;
import :error;
import :debug;
import :data_input;
import :data_output;

import std;

namespace lucene {

namespace {

const IntsRef &as_ints_ref(const std::any &value) {
    static const IntsRef empty;
    if (value.type() != typeid(IntsRef)) {
        return empty;
    }
    return std::any_cast<const IntsRef &>(value);
}

IntsRef copy_of(std::span<const int32_t> ints) {
    return IntsRef(std::vector<int32_t>(ints.begin(), ints.end()));
}

} // namespace

const FstIntSequenceOutputs &FstIntSequenceOutputs::singleton() {
    static const FstIntSequenceOutputs instance;
    return instance;
}

std::any FstIntSequenceOutputs::common(const std::any &output1, const std::any &output2) const {
    const auto first = as_ints_ref(output1).as_slice();
    const auto second = as_ints_ref(output2).as_slice();

    size_t prefix = 0;
    const size_t limit = std::min(first.size(), second.size());
    while (prefix < limit && first[prefix] == second[prefix]) {
        ++prefix;
    }
    debug_check(prefix <= first.size() && prefix <= second.size(), "the common prefix cannot be longer");
    if (prefix == 0) {
        return get_no_output();
    }
    if (prefix == first.size()) {
        return std::any{copy_of(first)};
    }
    if (prefix == second.size()) {
        return std::any{copy_of(second)};
    }
    return std::any{copy_of(first.first(prefix))};
}

std::any FstIntSequenceOutputs::subtract(const std::any &output, const std::any &inc) const {
    const auto value = as_ints_ref(output).as_slice();
    const auto increment = as_ints_ref(inc).as_slice();
    if (increment.empty()) {
        return std::any{copy_of(value)};
    }
    debug_check(increment.size() <= value.size(), "the subtracted prefix must not be longer");
    debug_check(std::equal(increment.begin(), increment.end(), value.begin()), "inc must be a prefix of output");
    if (increment.size() == value.size()) {
        return get_no_output();
    }
    return std::any{copy_of(value.subspan(increment.size()))};
}

std::any FstIntSequenceOutputs::add(const std::any &prefix, const std::any &output) const {
    const auto first = as_ints_ref(prefix).as_slice();
    const auto second = as_ints_ref(output).as_slice();
    if (first.empty()) {
        return std::any{copy_of(second)};
    }
    if (second.empty()) {
        return std::any{copy_of(first)};
    }
    std::vector<int32_t> combined;
    combined.reserve(first.size() + second.size());
    combined.insert(combined.end(), first.begin(), first.end());
    combined.insert(combined.end(), second.begin(), second.end());
    return std::any{IntsRef(std::move(combined))};
}

VoidResult FstIntSequenceOutputs::write(const std::any &output, DataOutput &out) const {
    const auto ints = as_ints_ref(output).as_slice();
    if (auto result = out.write_vint(static_cast<int32_t>(ints.size())); !result.has_value()) {
        return result;
    }
    // Lucene stores the values with `writeVInt` as well (they are sorted ordinals).
    for (const int32_t value : ints) {
        if (auto result = out.write_vint(value); !result.has_value()) {
            return result;
        }
    }
    return {};
}

Result<std::any> FstIntSequenceOutputs::read(DataInput &input) const {
    auto length = input.read_vint();
    if (!length.has_value()) {
        return err(length.error());
    }
    if (length.value() == 0) {
        return get_no_output();
    }
    if (length.value() < 0) {
        return err(LuceneError::corrupt_index("negative int sequence length"));
    }
    std::vector<int32_t> ints;
    ints.reserve(static_cast<size_t>(length.value()));
    for (int32_t i = 0; i < length.value(); ++i) {
        auto value = input.read_vint();
        if (!value.has_value()) {
            return err(value.error());
        }
        ints.push_back(value.value());
    }
    return std::any{IntsRef(std::move(ints))};
}

std::any FstIntSequenceOutputs::get_no_output() const {
    return std::any{IntsRef()};
}

bool FstIntSequenceOutputs::is_no_output(const std::any &output) const {
    return as_ints_ref(output).length() == 0;
}

std::string FstIntSequenceOutputs::output_to_string(const std::any &output) const {
    return as_ints_ref(output).to_string();
}

int64_t FstIntSequenceOutputs::ram_bytes_used(const std::any &output) const {
    return static_cast<int64_t>(as_ints_ref(output).ints().size() * sizeof(int32_t));
}

} // namespace lucene
