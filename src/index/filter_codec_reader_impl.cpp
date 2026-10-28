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

module lucene_core:filter_codec_reader.impl;

import :filter_codec_reader;
import :types;
import :bits;
import :codec_reader;
import :error;

import std;

namespace lucene {

FilterCodecReaderBits::FilterCodecReaderBits(std::shared_ptr<const Bits> bits)
    : hard_(std::move(bits)), wrapped_(nullptr), mixed_(false) {}

FilterCodecReaderBits::FilterCodecReaderBits(std::shared_ptr<const Bits> hard, std::shared_ptr<const Bits> wrapped,
                                             bool mixed)
    : hard_(std::move(hard)), wrapped_(std::move(wrapped)), mixed_(mixed) {}

std::shared_ptr<FilterCodecReaderBits> FilterCodecReaderBits::mixed(std::shared_ptr<const Bits> hard_live_docs,
                                                                   std::shared_ptr<const Bits> wrapped_live_docs) {
    return std::shared_ptr<FilterCodecReaderBits>(
            new FilterCodecReaderBits(std::move(hard_live_docs), std::move(wrapped_live_docs), true));
}

Result<bool> FilterCodecReaderBits::get(size_t index) const {
    if (!mixed_) {
        return hard_->get(index);
    }
    auto hard = hard_->get(index);
    if (!hard) {
        return std::unexpected(std::move(hard.error()));
    }
    if (!hard.value()) {
        return false;
    }
    return wrapped_->get(index);
}

size_t FilterCodecReaderBits::length() const { return hard_->length(); }

std::string FilterCodecReaderBits::to_string() const {
    if (!mixed_) {
        return "FilterCodecReaderBits(" + hard_->to_string() + ")";
    }
    return "FilterCodecReaderBits(mixed " + hard_->to_string() + ", " + wrapped_->to_string() + ")";
}

FilterCodecReader::FilterCodecReader(std::shared_ptr<CodecReader> delegate) : delegate_(std::move(delegate)) {
    if (delegate_ == nullptr) {
        throw std::invalid_argument("a filter codec reader needs a delegate");
    }
}

std::string FilterCodecReader::to_string() const {
    auto inner = delegate_->to_string();
    return "FilterCodecReader(" + inner + ")";
}

} // namespace lucene
