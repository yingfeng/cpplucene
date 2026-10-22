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

module lucene_core:fst_byte_sequence_outputs.impl;

import :fst_byte_sequence_outputs;
import :fst_outputs;
import :types;
import :error;
import :debug;
import :core_helper;
import :bytes_ref;
import :data_input;
import :data_output;

import std;

namespace lucene {

namespace {

const BytesRef &as_bytes_ref(const std::any &value) {
    static const BytesRef empty;
    if (value.type() != typeid(BytesRef)) {
        return empty;
    }
    return std::any_cast<const BytesRef &>(value);
}

BytesRef copy_of(std::span<const uint8_t> bytes) {
    return BytesRef(std::vector<uint8_t>(bytes.begin(), bytes.end()));
}

bool starts_with(std::span<const uint8_t> bytes, std::span<const uint8_t> prefix) {
    return bytes.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), bytes.begin());
}

} // namespace

const FstByteSequenceOutputs &FstByteSequenceOutputs::singleton() {
    static const FstByteSequenceOutputs instance;
    return instance;
}

std::any FstByteSequenceOutputs::common(const std::any &output1, const std::any &output2) const {
    const auto first = as_bytes_ref(output1).as_slice();
    const auto second = as_bytes_ref(output2).as_slice();
    const int32_t mismatch = CoreHelper::mismatch_u8(first, second);
    if (mismatch < 0) {
        // Identical.
        return std::any{copy_of(first)};
    }
    if (mismatch == 0) {
        return get_no_output();
    }
    if (static_cast<size_t>(mismatch) == first.size()) {
        return std::any{copy_of(first)};
    }
    if (static_cast<size_t>(mismatch) == second.size()) {
        return std::any{copy_of(second)};
    }
    return std::any{copy_of(first.first(static_cast<size_t>(mismatch)))};
}

std::any FstByteSequenceOutputs::subtract(const std::any &output, const std::any &inc) const {
    const auto value = as_bytes_ref(output).as_slice();
    const auto increment = as_bytes_ref(inc).as_slice();
    if (increment.empty()) {
        // No prefix removed.
        return std::any{copy_of(value)};
    }
    debug_check(starts_with(value, increment), "the subtracted output must be a prefix of the output");
    if (increment.size() == value.size()) {
        return get_no_output();
    }
    debug_check(increment.size() < value.size(), "the increment must be shorter than the output");
    return std::any{copy_of(value.subspan(increment.size()))};
}

std::any FstByteSequenceOutputs::add(const std::any &prefix, const std::any &output) const {
    const auto first = as_bytes_ref(prefix).as_slice();
    const auto second = as_bytes_ref(output).as_slice();
    if (first.empty()) {
        return std::any{copy_of(second)};
    }
    if (second.empty()) {
        return std::any{copy_of(first)};
    }
    std::vector<uint8_t> combined;
    combined.reserve(first.size() + second.size());
    combined.insert(combined.end(), first.begin(), first.end());
    combined.insert(combined.end(), second.begin(), second.end());
    return std::any{BytesRef(std::move(combined))};
}

VoidResult FstByteSequenceOutputs::write(const std::any &output, DataOutput &out) const {
    const auto bytes = as_bytes_ref(output).as_slice();
    if (auto result = out.write_vint(static_cast<int32_t>(bytes.size())); !result.has_value()) {
        return result;
    }
    return out.write_bytes(bytes);
}

Result<std::any> FstByteSequenceOutputs::read(DataInput &input) const {
    auto length = input.read_vint();
    if (!length.has_value()) {
        return err(length.error());
    }
    if (length.value() == 0) {
        return get_no_output();
    }
    if (length.value() < 0) {
        return err(LuceneError::corrupt_index("negative output length"));
    }
    auto bytes = input.get_bytes(static_cast<size_t>(length.value()));
    if (!bytes.has_value()) {
        return err(bytes.error());
    }
    return std::any{BytesRef(std::move(bytes.value()))};
}

VoidResult FstByteSequenceOutputs::skip_output(DataInput &input) const {
    auto length = input.read_vint();
    if (!length.has_value()) {
        return err(length.error());
    }
    if (length.value() == 0) {
        return {};
    }
    return input.skip_bytes(length.value());
}

std::any FstByteSequenceOutputs::get_no_output() const {
    return std::any{BytesRef()};
}

bool FstByteSequenceOutputs::is_no_output(const std::any &output) const {
    return as_bytes_ref(output).length() == 0;
}

std::string FstByteSequenceOutputs::output_to_string(const std::any &output) const {
    return as_bytes_ref(output).to_display_string();
}

int64_t FstByteSequenceOutputs::ram_bytes_used(const std::any &output) const {
    return static_cast<int64_t>(as_bytes_ref(output).bytes().size());
}

} // namespace lucene
