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

module lucene_core:direct_reader.impl;

import :direct_reader;
import :direct_writer;
import :packed_ints;
import :types;
import :error;
import :debug;
import :random_access_input;

import std;

namespace lucene {

DirectReader::DirectReader(const RandomAccessInput &input, size_t base_offset, int32_t bits_per_value)
    : input_(&input), base_offset_(base_offset), bits_per_value_(bits_per_value) {
    debug_check(bits_per_value >= 1 && bits_per_value <= 64, "bitsPerValue must be in [1, 64]");
}

Result<DirectReader> DirectReader::get_instance(const RandomAccessInput &input, int32_t bits_per_value) {
    if (!std::binary_search(DirectWriter::SUPPORTED_BITS_PER_VALUE.begin(),
                            DirectWriter::SUPPORTED_BITS_PER_VALUE.end(), bits_per_value)) {
        return err(LuceneError::illegal_argument("Unsupported bitsPerValue " + std::to_string(bits_per_value)));
    }
    return DirectReader(input, 0, bits_per_value);
}

Result<int64_t> DirectReader::get(size_t index) const {
    const size_t bpv = static_cast<size_t>(bits_per_value_);
    const size_t start_bit = index * bpv;
    const size_t first_byte = start_bit / 8;
    const size_t last_byte = (start_bit + bpv - 1) / 8;
    const size_t num_bytes = last_byte - first_byte + 1;
    const size_t shift = start_bit & 7;
    debug_check(num_bytes <= 8, "a single value spans at most 8 bytes");

    std::array<uint8_t, 8> buffer{};
    if (auto result = input_->read_bytes(base_offset_ + first_byte, std::span<uint8_t>(buffer.data(), num_bytes));
        !result.has_value()) {
        return err(result.error());
    }
    uint64_t word = 0;
    for (size_t i = 0; i < num_bytes; ++i) {
        word |= static_cast<uint64_t>(buffer[i]) << (8 * i);
    }
    const uint64_t mask =
        bits_per_value_ >= 64 ? std::numeric_limits<uint64_t>::max() : ((uint64_t{1} << bpv) - 1);
    return static_cast<int64_t>((word >> shift) & mask);
}

} // namespace lucene
