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

module lucene_core:to_string_utils.impl;

import :to_string_utils;

import std;

import :bytes_ref;
import :types;

namespace lucene {

std::string ToStringUtils::long_hex(uint64_t x) { return std::format("0x{:016x}", x); }

std::string ToStringUtils::bytes_ref_to_string(const BytesRef &b) {
    if (b.length() == 0) {
        return "null";
    }
    auto text = b.utf8_to_string();
    if (text.has_value()) {
        return text.value();
    }
    // Not valid UTF-8 (or a prefix that ends in the middle of a code point): fall
    // back to the byte rendering, like the reference implementation does.
    return b.to_display_string();
}

std::string ToStringUtils::bytes_ref_to_string_from_bytes(std::span<const uint8_t> bytes) {
    BytesRef ref;
    ref.copy_from_slice(bytes);
    return bytes_ref_to_string(ref);
}

} // namespace lucene
