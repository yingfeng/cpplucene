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

module lucene_core:index_output.impl;

import :index_output;
import :types;
import :bit_util;

import std;

namespace lucene {

Result<size_t> IndexOutput::align_file_pointer(size_t alignment_bytes) {
    if (alignment_bytes == 0 || !BitUtil::is_zero_or_power_of_two(static_cast<int32_t>(alignment_bytes))) {
        return err(LuceneError::illegal_argument(
            std::format("alignment_bytes must be a power of two, got {}", alignment_bytes)));
    }
    auto pointer = get_file_pointer();
    if (!pointer) {
        return std::unexpected(std::move(pointer.error()));
    }
    const size_t pos = pointer.value();
    const size_t padding = (alignment_bytes - (pos % alignment_bytes)) % alignment_bytes;
    for (size_t i = 0; i < padding; ++i) {
        if (auto r = write_byte(0); !r) {
            return std::unexpected(std::move(r.error()));
        }
    }
    return padding;
}

} // namespace lucene
