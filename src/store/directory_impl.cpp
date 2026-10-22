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

module lucene_core:directory.impl;

import :directory;
import :types;
import :error;

import std;

namespace lucene {

namespace {
constexpr size_t COPY_BUFFER_SIZE = 64 * 1024;
} // namespace

VoidResult Directory::copy_from(const Directory &from,
                                std::string_view src,
                                std::string_view dest,
                                const IOContext &context) {
    auto input = from.open_input(src, context);
    if (!input) {
        return std::unexpected(std::move(input.error()));
    }
    auto output = create_output(dest, context);
    if (!output) {
        return std::unexpected(std::move(output.error()));
    }

    std::vector<uint8_t> buffer(COPY_BUFFER_SIZE);
    auto length = input.value()->length();
    if (!length) {
        return std::unexpected(std::move(length.error()));
    }
    size_t left = length.value();
    while (left > 0) {
        const size_t chunk = std::min(left, COPY_BUFFER_SIZE);
        auto view = std::span<uint8_t>(buffer.data(), chunk);
        if (auto r = input.value()->read_bytes(view); !r) {
            return r;
        }
        if (auto r = output.value()->write_bytes(view); !r) {
            return r;
        }
        left -= chunk;
    }
    if (auto r = output.value()->close(); !r) {
        return r;
    }
    return {};
}

} // namespace lucene
