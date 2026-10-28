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

export module lucene_core:to_string_utils;

import :bytes_ref;
import :types;

import std;

namespace lucene {

/// String renderings shared by the log and `explain` output.
///
/// Port of `util/to_string_utils.rs`.
export class ToStringUtils {
public:
    /// `0x` followed by the 16 hex digits of `x`.
    [[nodiscard]] static std::string long_hex(uint64_t x);

    /// The UTF-8 text of `b`, `"null"` when it is empty, or the reference
    /// implementation's byte rendering (`[61 62]`) when the bytes are not UTF-8.
    [[nodiscard]] static std::string bytes_ref_to_string(const BytesRef &b);

    /// [`bytes_ref_to_string`] for a byte vector.
    [[nodiscard]] static std::string bytes_ref_to_string_from_bytes(std::span<const uint8_t> bytes);
};

} // namespace lucene
