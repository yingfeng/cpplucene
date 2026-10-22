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

module lucene_core:ints_ref.impl;

import :ints_ref;
import :types;
import :error;
import :debug;

import std;

namespace lucene {

IntsRef::IntsRef(std::vector<int32_t> ints) : ints_(std::move(ints)), length_(ints_.size()) {}

IntsRef IntsRef::from_slice(std::vector<int32_t> ints, size_t offset, size_t length) {
    IntsRef result(std::move(ints));
    result.offset_ = offset;
    result.length_ = length;
    return result;
}

IntsRef IntsRef::deep_copy_of(const IntsRef &other) {
    const auto slice = other.as_slice();
    return IntsRef(std::vector<int32_t>(slice.begin(), slice.end()));
}

std::span<const int32_t> IntsRef::as_slice() const {
    debug_check(offset_ + length_ <= ints_.size(), "the active range must fit the backing array");
    return std::span<const int32_t>(ints_.data() + offset_, length_);
}

std::span<int32_t> IntsRef::as_mutable_slice() {
    debug_check(offset_ + length_ <= ints_.size(), "the active range must fit the backing array");
    return std::span<int32_t>(ints_.data() + offset_, length_);
}

int32_t IntsRef::compare_to(const IntsRef &other) const {
    const auto first = as_slice();
    const auto second = other.as_slice();
    const size_t common = std::min(first.size(), second.size());
    for (size_t i = 0; i < common; ++i) {
        if (first[i] != second[i]) {
            return first[i] < second[i] ? -1 : 1;
        }
    }
    if (first.size() == second.size()) {
        return 0;
    }
    return first.size() < second.size() ? -1 : 1;
}

VoidResult IntsRef::is_valid() const {
    if (offset_ + length_ > ints_.size()) {
        return err(LuceneError::illegal_state("offset + length must not exceed the ints length"));
    }
    return {};
}

std::string IntsRef::to_string() const {
    std::string result = "[";
    const auto slice = as_slice();
    for (size_t i = 0; i < slice.size(); ++i) {
        if (i != 0) {
            result.append(", ");
        }
        result.append(std::to_string(slice[i]));
    }
    result.append("]");
    return result;
}

int32_t IntsRefBuilder::int_at(size_t offset) const {
    debug_check(offset < length_, "offset out of bounds");
    return ints_[offset];
}

void IntsRefBuilder::set_int_at(size_t offset, int32_t value) {
    debug_check(offset < length_, "offset out of bounds");
    ints_[offset] = value;
}

VoidResult IntsRefBuilder::append(int32_t value) {
    if (length_ == ints_.size()) {
        ints_.push_back(value);
    } else {
        ints_[length_] = value;
    }
    ++length_;
    return {};
}

VoidResult IntsRefBuilder::grow(size_t new_length) {
    if (new_length > ints_.size()) {
        ints_.resize(new_length, 0);
    }
    length_ = new_length;
    return {};
}

VoidResult IntsRefBuilder::grow_no_copy(size_t new_length) {
    if (new_length > ints_.size()) {
        ints_.resize(new_length, 0);
    }
    length_ = new_length;
    return {};
}

VoidResult IntsRefBuilder::copy_ints(std::span<const int32_t> other, size_t other_offset, size_t other_length) {
    if (other_offset + other_length > other.size()) {
        return err(LuceneError::array_index_out_of_bounds("the source range is out of bounds"));
    }
    if (other_length > ints_.size()) {
        ints_.resize(other_length, 0);
    }
    std::copy_n(other.begin() + static_cast<std::vector<int32_t>::difference_type>(other_offset), other_length,
                ints_.begin());
    length_ = other_length;
    return {};
}

VoidResult IntsRefBuilder::copy_ints_ref(const IntsRef &other) {
    return copy_ints(other.as_slice(), 0, other.length());
}

IntsRef IntsRefBuilder::to_ints_ref() const {
    return IntsRef(std::vector<int32_t>(ints_.begin(),
                                        ints_.begin() + static_cast<std::vector<int32_t>::difference_type>(length_)));
}

} // namespace lucene
