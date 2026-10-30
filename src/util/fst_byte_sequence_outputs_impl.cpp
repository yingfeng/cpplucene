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

/// `FstOutput::as` already answers an empty `BytesRef` for a slot that holds
/// nothing or another type, which is this algebra's NO_OUTPUT.
const BytesRef &as_bytes_ref(const FstOutput &value) { return value.as<BytesRef>(); }

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

FstOutput FstByteSequenceOutputs::common(const FstOutput &output1, const FstOutput &output2) const {
    const auto first = as_bytes_ref(output1).as_slice();
    const auto second = as_bytes_ref(output2).as_slice();
    const int32_t mismatch = CoreHelper::mismatch_u8(first, second);
    if (mismatch < 0) {
        // Identical.
        return FstOutput{copy_of(first)};
    }
    if (mismatch == 0) {
        return get_no_output();
    }
    if (static_cast<size_t>(mismatch) == first.size()) {
        return FstOutput{copy_of(first)};
    }
    if (static_cast<size_t>(mismatch) == second.size()) {
        return FstOutput{copy_of(second)};
    }
    return FstOutput{copy_of(first.first(static_cast<size_t>(mismatch)))};
}

FstOutput FstByteSequenceOutputs::subtract(const FstOutput &output, const FstOutput &inc) const {
    const auto value = as_bytes_ref(output).as_slice();
    const auto increment = as_bytes_ref(inc).as_slice();
    if (increment.empty()) {
        // No prefix removed.
        return FstOutput{copy_of(value)};
    }
    debug_check(starts_with(value, increment), "the subtracted output must be a prefix of the output");
    if (increment.size() == value.size()) {
        return get_no_output();
    }
    debug_check(increment.size() < value.size(), "the increment must be shorter than the output");
    return FstOutput{copy_of(value.subspan(increment.size()))};
}

FstOutput FstByteSequenceOutputs::add(const FstOutput &prefix, const FstOutput &output) const {
    const auto first = as_bytes_ref(prefix).as_slice();
    const auto second = as_bytes_ref(output).as_slice();
    if (first.empty()) {
        return FstOutput{copy_of(second)};
    }
    if (second.empty()) {
        return FstOutput{copy_of(first)};
    }
    std::vector<uint8_t> combined;
    combined.reserve(first.size() + second.size());
    combined.insert(combined.end(), first.begin(), first.end());
    combined.insert(combined.end(), second.begin(), second.end());
    return FstOutput{BytesRef(std::move(combined))};
}

VoidResult FstByteSequenceOutputs::write(const FstOutput &output, DataOutput &out) const {
    const auto bytes = as_bytes_ref(output).as_slice();
    if (auto result = out.write_vint(static_cast<int32_t>(bytes.size())); !result.has_value()) {
        return result;
    }
    return out.write_bytes(bytes);
}

Result<FstOutput> FstByteSequenceOutputs::read(DataInput &input) const {
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
    return FstOutput{BytesRef(std::move(bytes.value()))};
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

FstOutput FstByteSequenceOutputs::get_no_output() const {
    return FstOutput{BytesRef()};
}

bool FstByteSequenceOutputs::is_no_output(const FstOutput &output) const {
    return as_bytes_ref(output).length() == 0;
}

std::string FstByteSequenceOutputs::output_to_string(const FstOutput &output) const {
    return as_bytes_ref(output).to_display_string();
}

int64_t FstByteSequenceOutputs::ram_bytes_used(const FstOutput &output) const {
    return static_cast<int64_t>(as_bytes_ref(output).bytes().size());
}

} // namespace lucene
