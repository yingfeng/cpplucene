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

module lucene_core:check_sum_index_input.impl;

import :check_sum_index_input;
import :error;
import :index_input;
import :types;

import std;

namespace lucene {

VoidResult CheckSumIndexInput::seek(size_t pos) {
    auto current = get_file_pointer();
    if (!current) {
        return std::unexpected(std::move(current.error()));
    }
    if (pos < current.value()) {
        return err(LuceneError::illegal_state(
                std::format("cannot seek backwards (pos={} getFilePointer()={})", pos, current.value())));
    }
    return skip_by_reading(pos - current.value());
}

VoidResult CheckSumIndexInput::skip_by_reading(size_t num_bytes) {
    std::vector<uint8_t> skip_buffer(SKIP_BUFFER_SIZE, 0);
    size_t skipped = 0;
    while (skipped < num_bytes) {
        const size_t step = std::min(SKIP_BUFFER_SIZE, num_bytes - skipped);
        const std::span<uint8_t> target(skip_buffer.data(), step);
        if (auto read = read_bytes_with_buffer(target, false); !read) {
            return read;
        }
        skipped += step;
    }
    return {};
}

} // namespace lucene
