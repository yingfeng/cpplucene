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

module lucene_core:read_write_data_output.impl;

import :read_write_data_output;
import :reverse_bytes_reader;
import :types;
import :error;
import :debug;
import :data_output;

import std;

namespace lucene {

ReadWriteDataOutput::ReadWriteDataOutput(int32_t block_bits, bool contiguous_reader) {
    // Kept for signature compatibility with the Rust port; a contiguous buffer
    // does not need block sizing.
    (void)block_bits;
    (void)contiguous_reader;
}

VoidResult ReadWriteDataOutput::write_byte(uint8_t b) {
    debug_check(!frozen_, "cannot write after freeze()");
    bytes_.push_back(b);
    return {};
}

VoidResult ReadWriteDataOutput::write_bytes(std::span<const uint8_t> b, size_t offset, size_t length) {
    debug_check(!frozen_, "cannot write after freeze()");
    if (offset + length > b.size()) {
        return err(LuceneError::illegal_argument("write out of bounds"));
    }
    bytes_.insert(bytes_.end(), b.begin() + static_cast<std::vector<uint8_t>::difference_type>(offset),
                  b.begin() + static_cast<std::vector<uint8_t>::difference_type>(offset + length));
    return {};
}

VoidResult ReadWriteDataOutput::freeze() {
    frozen_ = true;
    return {};
}

Result<ReverseBytesReader> ReadWriteDataOutput::get_reverse_bytes_reader() const {
    if (!frozen_) {
        return err(LuceneError::illegal_state(
            "Call ReadWriteDataOutput#freeze before ReadWriteDataOutput#get_reverse_bytes_reader"));
    }
    return ReverseBytesReader(bytes_);
}

VoidResult ReadWriteDataOutput::write_to(DataOutput &out) const {
    if (!frozen_) {
        return err(LuceneError::illegal_state("Call ReadWriteDataOutput#freeze before write_to"));
    }
    return out.write_bytes(bytes_);
}

void ReadWriteDataOutput::reset() noexcept {
    bytes_.clear();
    frozen_ = false;
}

} // namespace lucene
