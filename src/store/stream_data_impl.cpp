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

module lucene_core:stream_data.impl;

import :stream_data;
import :types;
import std;

import :data_input;
import :data_output;
import :error;

namespace lucene {

VoidResult OutputStreamDataOutput::write_byte(uint8_t b) {
    out_->put(static_cast<char>(b));
    if (!out_->good()) {
        return err(LuceneError::io("writing to the stream failed"));
    }
    return {};
}

VoidResult OutputStreamDataOutput::write_bytes(std::span<const uint8_t> b, size_t offset, size_t length) {
    if (offset > b.size() || length > b.size() - offset) {
        return err(LuceneError::array_index_out_of_bounds(
                std::format("offset {} + length {} exceeds {}", offset, length, b.size())));
    }
    out_->write(reinterpret_cast<const char *>(b.data() + offset), static_cast<std::streamsize>(length));
    if (!out_->good()) {
        return err(LuceneError::io("writing to the stream failed"));
    }
    return {};
}

Result<uint8_t> InputStreamDataInput::read_byte() {
    const int value = in_->get();
    if (value == std::char_traits<char>::eof()) {
        return err(LuceneError::io("reading from the stream failed (end of stream)"));
    }
    return static_cast<uint8_t>(value);
}

VoidResult InputStreamDataInput::read_bytes(std::span<uint8_t> b) {
    if (b.empty()) {
        return {};
    }
    in_->read(reinterpret_cast<char *>(b.data()), static_cast<std::streamsize>(b.size()));
    if (in_->gcount() != static_cast<std::streamsize>(b.size())) {
        return err(LuceneError::io("reading from the stream failed (end of stream)"));
    }
    return {};
}

VoidResult InputStreamDataInput::skip_bytes(int64_t num_bytes) {
    if (num_bytes < 0) {
        return err(LuceneError::illegal_argument("num_bytes must be >= 0"));
    }
    in_->ignore(static_cast<std::streamsize>(num_bytes));
    if (in_->gcount() != static_cast<std::streamsize>(num_bytes)) {
        return err(LuceneError::io("skipping over the stream failed (end of stream)"));
    }
    return {};
}

} // namespace lucene
