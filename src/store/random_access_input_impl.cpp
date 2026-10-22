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

module lucene_core:random_access_input.impl;

import :random_access_input;
import :types;
import :core_helper;
import :bit_util;

import std;

namespace lucene {

VoidResult RandomAccessInput::read_ints(size_t pos, std::span<int32_t> dst, size_t offset, size_t len) const {
    if (auto check = CoreHelper::check_from_index_size(offset, len, dst.size()); !check) {
        return std::unexpected(std::move(check.error()));
    }
    for (size_t i = 0; i < len; ++i) {
        auto value = read_int(pos + i * BitUtil::INT_BYTES);
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        dst[offset + i] = value.value();
    }
    return {};
}

VoidResult RandomAccessInput::read_longs(size_t pos, std::span<int64_t> dst, size_t offset, size_t len) const {
    if (auto check = CoreHelper::check_from_index_size(offset, len, dst.size()); !check) {
        return std::unexpected(std::move(check.error()));
    }
    for (size_t i = 0; i < len; ++i) {
        auto value = read_long(pos + i * BitUtil::LONG_BYTES);
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        dst[offset + i] = value.value();
    }
    return {};
}

VoidResult RandomAccessInput::read_floats(size_t pos, std::span<float> dst, size_t offset, size_t len) const {
    if (auto check = CoreHelper::check_from_index_size(offset, len, dst.size()); !check) {
        return std::unexpected(std::move(check.error()));
    }
    for (size_t i = 0; i < len; ++i) {
        auto value = read_int(pos + i * BitUtil::INT_BYTES);
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        dst[offset + i] = std::bit_cast<float>(static_cast<uint32_t>(value.value()));
    }
    return {};
}

} // namespace lucene
