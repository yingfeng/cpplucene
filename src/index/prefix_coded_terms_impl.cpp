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

module lucene_core:prefix_coded_terms.impl;

import :prefix_coded_terms;

import std;

import :byte_array_output;
import :bytes_ref;
import :data_input;
import :data_output;
import :error;
import :index_output;
import :string_helper;
import :types;

namespace lucene {

namespace {

/// The encoded content, empty when the terms carry none.
std::span<const uint8_t> content_of(const std::shared_ptr<const std::vector<uint8_t>> &backing) {
    if (backing == nullptr) {
        return {};
    }
    return *backing;
}

} // namespace

// ---------------------------------------------------------------- Builder

VoidResult PrefixCodedTerms::Builder::add(std::string_view field, const BytesRef &term) {
    size_t prefix = 0;
    if (size_ > 0 && last_term_.has_value()) {
        const int32_t field_compare = field.compare(last_field_);
        if (field_compare < 0) {
            return err(LuceneError::illegal_argument("terms have to be added in increasing field order"));
        }
        if (field_compare == 0) {
            if (last_term_.value().compare_to(term) >= 0) {
                return err(LuceneError::illegal_argument("terms have to be added in increasing order"));
            }
            auto common = StringHelper::bytes_difference(last_term_.value().as_slice(), term.as_slice());
            if (!common.has_value()) {
                return std::unexpected(std::move(common.error()));
            }
            prefix = common.value();
        }
    }

    if (size_ > 0 && field == last_field_) {
        if (auto written = output_.write_vint(static_cast<int32_t>(prefix << 1)); !written.has_value()) {
            return written;
        }
    } else {
        // A new field: flag it and store the field name.
        if (auto written = output_.write_vint(1); !written.has_value()) {
            return written;
        }
        if (auto written = output_.write_string(field); !written.has_value()) {
            return written;
        }
        prefix = 0;
    }

    const size_t suffix = term.length() - prefix;
    if (auto written = output_.write_vint(static_cast<int32_t>(suffix)); !written.has_value()) {
        return written;
    }
    if (auto written = output_.write_bytes(term.as_slice(), prefix, suffix); !written.has_value()) {
        return written;
    }

    last_field_ = std::string(field);
    last_term_ = term;
    ++size_;
    return {};
}

PrefixCodedTerms PrefixCodedTerms::Builder::build() const {
    PrefixCodedTerms terms;
    const std::span<const uint8_t> bytes = output_.bytes();
    terms.content_ = std::make_shared<const std::vector<uint8_t>>(bytes.begin(), bytes.end());
    terms.size_ = size_;
    return terms;
}

// ------------------------------------------------------------ TermIterator

TermIterator::TermIterator(std::shared_ptr<const std::vector<uint8_t>> content, int64_t del_gen)
        : content_(std::move(content)), del_gen_(del_gen) {
    if (content_ != nullptr) {
        input_ = ByteArrayDataInput(std::span<const uint8_t>(*content_));
    }
}

Result<const BytesRef *> TermIterator::next() {
    if (content_ == nullptr || input_.eof()) {
        field_.clear();
        return static_cast<const BytesRef *>(nullptr);
    }
    auto code = input_.read_vint();
    if (!code.has_value()) {
        return std::unexpected(std::move(code.error()));
    }
    if (code.value() < 0) {
        return err(LuceneError::illegal_state("invalid prefix code in the term data"));
    }
    if ((code.value() & 1) != 0) {
        // A new field name follows.
        auto field = input_.read_string();
        if (!field.has_value()) {
            return std::unexpected(std::move(field.error()));
        }
        field_ = std::move(field.value());
    } else if (field_.empty()) {
        return err(LuceneError::illegal_state("the term data does not name a field"));
    }

    const size_t prefix = static_cast<size_t>(code.value() >> 1);
    auto suffix_result = input_.read_vint();
    if (!suffix_result.has_value()) {
        return std::unexpected(std::move(suffix_result.error()));
    }
    if (suffix_result.value() < 0) {
        return err(LuceneError::illegal_state("invalid suffix length in the term data"));
    }
    const size_t suffix = static_cast<size_t>(suffix_result.value());
    if (prefix > term_.length()) {
        return err(LuceneError::illegal_state("the term data shares more prefix than the previous term has"));
    }

    term_.bytes().resize(prefix + suffix);
    if (suffix > 0) {
        std::span<uint8_t> destination(term_.bytes().data() + prefix, suffix);
        if (auto read = input_.read_bytes(destination); !read.has_value()) {
            return std::unexpected(std::move(read.error()));
        }
    }
    term_.set_offset(0);
    term_.set_length(prefix + suffix);
    return &term_;
}

// --------------------------------------------------------- PrefixCodedTerms

TermIterator PrefixCodedTerms::iterator() const { return TermIterator(content_, del_gen_); }

bool PrefixCodedTerms::operator==(const PrefixCodedTerms &other) const {
    if (size_ != other.size_) {
        return false;
    }
    return std::ranges::equal(content_of(content_), content_of(other.content_));
}

size_t PrefixCodedTerms::hash() const {
    size_t result = 0x9E3779B97F4A7C15ULL;
    const std::span<const uint8_t> bytes = content_of(content_);
    // FNV-1a over the encoded content; the encoding is deterministic.
    for (const uint8_t byte : bytes) {
        result ^= byte;
        result *= 1099511628211ULL;
    }
    result ^= static_cast<size_t>(size_);
    return result;
}

} // namespace lucene
