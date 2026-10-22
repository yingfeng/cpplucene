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

module lucene_core:block_tree_msb_vlong.impl;

import :block_tree_msb_vlong;

import :data_input;
import :data_output;
import :debug;
import :error;
import :types;

import std;

namespace lucene {

Result<int64_t> read_msb_vlong(DataInput &input) {
    int64_t value = 0;
    while (true) {
        auto byte = input.read_byte();
        if (!byte) {
            return std::unexpected(std::move(byte.error()));
        }
        value = (value << 7) | static_cast<int64_t>(byte.value() & 0x7F);
        if ((byte.value() & 0x80) == 0) {
            break;
        }
    }
    return value;
}

VoidResult write_msb_vlong(int64_t value, DataOutput &out) {
    debug_check(value >= 0);
    if (value < 0) {
        return err(LuceneError::illegal_argument(std::format("msb vlong must not be negative: {}", value)));
    }
    // Keep zero bits in the most significant byte so that sibling block
    // pointers share their prefix bytes.
    int32_t bits = 0;
    for (uint64_t remaining = static_cast<uint64_t>(value); remaining != 0; remaining >>= 1) {
        ++bits;
    }
    const size_t bytes_needed = bits <= 1 ? 1 : static_cast<size_t>((bits - 1) / 7 + 1);
    uint64_t shifted = static_cast<uint64_t>(value) << (64 - bytes_needed * 7);
    for (size_t i = 1; i < bytes_needed; ++i) {
        const uint8_t byte = static_cast<uint8_t>(((shifted >> 57) & 0x7F) | 0x80);
        if (auto written = out.write_byte(byte); !written) {
            return written;
        }
        shifted <<= 7;
    }
    const uint8_t last_byte = static_cast<uint8_t>((shifted >> 57) & 0x7F);
    return out.write_byte(last_byte);
}

} // namespace lucene
