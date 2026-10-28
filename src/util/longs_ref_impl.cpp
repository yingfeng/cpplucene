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

module lucene_core:longs_ref.impl;

import :longs_ref;
import :types;
import :error;

import std;

namespace lucene {

LongsRef::LongsRef(std::vector<int64_t> longs) : longs_(std::move(longs)), length_(longs_.size()) {}

LongsRef LongsRef::from_slice(std::vector<int64_t> longs, size_t offset, size_t length) {
    LongsRef ref;
    ref.longs_ = std::move(longs);
    ref.offset_ = offset;
    ref.length_ = length;
    return ref;
}

LongsRef LongsRef::deep_copy_of(const LongsRef &other) {
    LongsRef copy;
    copy.longs_.assign(other.as_slice().begin(), other.as_slice().end());
    copy.length_ = copy.longs_.size();
    return copy;
}

std::span<const int64_t> LongsRef::as_slice() const {
    if (offset_ > longs_.size()) {
        return {};
    }
    const size_t available = longs_.size() - offset_;
    return std::span<const int64_t>(longs_).subspan(offset_, std::min(length_, available));
}

std::span<int64_t> LongsRef::as_mutable_slice() {
    if (offset_ > longs_.size()) {
        return {};
    }
    const size_t available = longs_.size() - offset_;
    return std::span<int64_t>(longs_).subspan(offset_, std::min(length_, available));
}

int32_t LongsRef::compare_to(const LongsRef &other) const {
    return LongsRefComparator::compare(as_slice(), other.as_slice());
}

VoidResult LongsRef::is_valid() const {
    if (offset_ + length_ > longs_.size()) {
        return err(LuceneError::illegal_argument(std::format("offset {} + length {} exceeds the {} longs", offset_,
                                                            length_, longs_.size())));
    }
    return {};
}

std::string LongsRef::to_string() const {
    std::string text = "[";
    const auto values = as_slice();
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            text += ' ';
        }
        text += std::to_string(values[i]);
    }
    text += ']';
    return text;
}

int32_t LongsRefComparator::compare(std::span<const int64_t> left, std::span<const int64_t> right) {
    const size_t common = std::min(left.size(), right.size());
    for (size_t i = 0; i < common; ++i) {
        if (left[i] < right[i]) {
            return -1;
        }
        if (left[i] > right[i]) {
            return 1;
        }
    }
    if (left.size() < right.size()) {
        return -1;
    }
    return left.size() > right.size() ? 1 : 0;
}

} // namespace lucene
