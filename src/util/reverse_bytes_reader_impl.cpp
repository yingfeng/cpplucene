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

module lucene_core:reverse_bytes_reader.impl;

import :reverse_bytes_reader;
import :types;
import :error;
import :debug;
import :data_input;
import :fst_bytes_reader;

import std;

namespace lucene {

Result<uint8_t> ReverseBytesReader::read_byte() {
    if (pos_ < 0 || static_cast<size_t>(pos_) >= bytes_.size()) {
        return err(LuceneError::eof("read past the end of the reverse reader"));
    }
    const uint8_t value = bytes_[static_cast<size_t>(pos_)];
    --pos_;
    return value;
}

VoidResult ReverseBytesReader::read_bytes(std::span<uint8_t> b) {
    if (pos_ < 0 || static_cast<size_t>(pos_) + 1 < b.size()) {
        return err(LuceneError::eof("read past the end of the reverse reader"));
    }
    for (size_t i = 0; i < b.size(); ++i) {
        debug_check(pos_ >= 0, "position must stay non-negative while reading");
        b[i] = bytes_[static_cast<size_t>(pos_)];
        --pos_;
    }
    return {};
}

VoidResult ReverseBytesReader::skip_bytes(int64_t num_bytes) {
    pos_ -= num_bytes;
    return {};
}

} // namespace lucene
