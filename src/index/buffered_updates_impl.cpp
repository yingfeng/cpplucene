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

module lucene_core:buffered_updates.impl;

import :buffered_updates;

import std;

import :byte_block_pool;
import :bytes_ref;
import :bytes_ref_hash;
import :doc_values_update;
import :error;
import :field_updates_buffer;
import :query;
import :term;
import :types;

namespace lucene {

namespace {

/// Initial capacity of a per-field term hash, i.e. `BytesRefHash::DEFAULT_CAPACITY`.
constexpr size_t kInitialCapacity = 16;

} // namespace

void BufferedUpdates::refresh_field_updates_bytes() {
    int64_t total = 0;
    for (const auto &[field, buffer] : field_updates_) {
        total += static_cast<int64_t>(field.size()) + buffer.ram_bytes_used();
    }
    field_updates_bytes_used_ = total;
}

// ---------------------------------------------------------------------------
// DeletedTerms::FieldTerms
// ---------------------------------------------------------------------------

DeletedTerms::FieldTerms::FieldTerms(size_t capacity)
    : hash(capacity, std::make_unique<DirectBytesStartArray>(capacity)), values() {}

// ---------------------------------------------------------------------------
// DeletedTerms
// ---------------------------------------------------------------------------

VoidResult DeletedTerms::put(const Term &term, int32_t value) {
    debug_check(value >= 0);
    auto field_entry = delete_terms_.find(term.field());
    if (field_entry == delete_terms_.end()) {
        bytes_used_ += static_cast<int64_t>(term.field().size());
        field_entry = delete_terms_.emplace(std::string(term.field()), FieldTerms(kInitialCapacity)).first;
    }
    FieldTerms &field_terms = field_entry->second;

    auto id = field_terms.hash.add(term.bytes(), pool_);
    if (!id.has_value()) {
        return std::unexpected(std::move(id.error()));
    }
    if (id.value() < 0) {
        // Already present: the id was returned as `-(id + 1)`.
        const size_t index = static_cast<size_t>(-id.value() - 1);
        if (index >= field_terms.values.size()) {
            return err(LuceneError::illegal_state("missing value slot for an existing deleted term"));
        }
        field_terms.values[index] = value;
        return {};
    }
    const size_t index = static_cast<size_t>(id.value());
    if (index >= field_terms.values.size()) {
        field_terms.values.resize(index + 1, 0);
    }
    field_terms.values[index] = value;
    ++terms_size_;
    return {};
}

Result<int32_t> DeletedTerms::get(const Term &term) const {
    const auto field_entry = delete_terms_.find(term.field());
    if (field_entry == delete_terms_.end()) {
        return -1;
    }
    const FieldTerms &field_terms = field_entry->second;
    auto id = field_terms.hash.find(term.bytes(), pool_);
    if (!id.has_value()) {
        return std::unexpected(std::move(id.error()));
    }
    if (id.value() == -1) {
        return -1;
    }
    const size_t index = static_cast<size_t>(id.value());
    if (index >= field_terms.values.size()) {
        return err(LuceneError::illegal_state(
                std::format("missing value for bytes reference id {}", id.value())));
    }
    return field_terms.values[index];
}

void DeletedTerms::clear() {
    pool_.reset(false, false);
    bytes_used_ = 0;
    delete_terms_.clear();
    terms_size_ = 0;
}

Result<std::vector<Term>> DeletedTerms::key_set() const {
    std::vector<Term> terms;
    terms.reserve(static_cast<size_t>(terms_size_));
    for (const auto &[field, field_terms] : delete_terms_) {
        for (size_t i = 0; i < field_terms.hash.size(); ++i) {
            auto position = field_terms.hash.get(static_cast<int32_t>(i), pool_);
            if (!position.has_value()) {
                return std::unexpected(std::move(position.error()));
            }
            const auto &block = pool_.buffer(position.value().block_index);
            BytesRef bytes;
            bytes.copy_from_slice(std::span<const uint8_t>(block.data() + position.value().offset,
                                                           position.value().length));
            terms.emplace_back(field, std::move(bytes));
        }
    }
    std::sort(terms.begin(), terms.end(), [](const Term &left, const Term &right) {
        const int32_t by_field = left.field().compare(right.field());
        return by_field != 0 ? by_field < 0 : left.bytes().compare_to(right.bytes()) < 0;
    });
    return terms;
}

VoidResult DeletedTerms::for_each_ordered(const Consumer &consumer) {
    for (auto &[field, field_terms] : delete_terms_) {
        if (auto sorted = field_terms.hash.sort(pool_); !sorted.has_value()) {
            return sorted;
        }
        const std::vector<int32_t> &ids = field_terms.hash.ids();
        for (size_t i = 0; i < field_terms.hash.size(); ++i) {
            const int32_t id = ids[i];
            auto position = field_terms.hash.get(id, pool_);
            if (!position.has_value()) {
                return std::unexpected(std::move(position.error()));
            }
            const auto &block = pool_.buffer(position.value().block_index);
            BytesRef bytes;
            bytes.copy_from_slice(
                    std::span<const uint8_t>(block.data() + position.value().offset, position.value().length));
            if (auto consumed = consumer(field, bytes, field_terms.values[static_cast<size_t>(id)]);
                !consumed.has_value()) {
                return consumed;
            }
        }
    }
    return {};
}

int64_t DeletedTerms::ram_bytes_used() const {
    int64_t total = bytes_used_ + static_cast<int64_t>(delete_terms_.size() * sizeof(FieldTerms));
    for (const auto &[field, field_terms] : delete_terms_) {
        total += static_cast<int64_t>(field.size());
        total += field_terms.hash.ram_bytes_used();
        total += static_cast<int64_t>(field_terms.values.size() * sizeof(int32_t));
    }
    return total;
}

std::string DeletedTerms::to_string() const {
    auto terms = key_set();
    if (!terms.has_value()) {
        return "{?}";
    }
    std::string out = "{";
    for (size_t i = 0; i < terms.value().size(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        const Term &term = terms.value()[i];
        auto count = get(term);
        out += term.field() + ":" + term.text() + "=" + (count.has_value() ? std::to_string(count.value()) : "?");
    }
    out += "}";
    return out;
}

// ---------------------------------------------------------------------------
// BufferedUpdates
// ---------------------------------------------------------------------------

VoidResult BufferedUpdates::add_binary_update(const DocValuesUpdate &update, int32_t doc_id_upto) {
    const auto term = update.buffered_term();
    if (!term.has_value()) {
        return std::unexpected(std::move(term.error()));
    }
    auto &buffer = field_updates_[update.field()];
    if (update.has_value()) {
        auto binary = update.binary_update();
        if (!binary.has_value()) {
            return std::unexpected(std::move(binary.error()));
        }
        auto value = binary.value()->value();
        if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
        }
        buffer.add_binary(*term.value(), update.field(), doc_id_upto, std::move(value.value()));
    } else {
        buffer.add_binary(*term.value(), update.field(), doc_id_upto, std::nullopt);
    }
    ++num_field_updates_;
    refresh_field_updates_bytes();
    return {};
}

VoidResult BufferedUpdates::add_numeric_update(const DocValuesUpdate &update, int32_t doc_id_upto) {
    const auto term = update.buffered_term();
    if (!term.has_value()) {
        return std::unexpected(std::move(term.error()));
    }
    auto &buffer = field_updates_[update.field()];
    if (update.has_value()) {
        auto numeric = update.numeric_update();
        if (!numeric.has_value()) {
            return std::unexpected(std::move(numeric.error()));
        }
        auto value = numeric.value()->value();
        if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
        }
        buffer.add_numeric(*term.value(), update.field(), doc_id_upto, value.value());
    } else {
        buffer.add_numeric(*term.value(), update.field(), doc_id_upto, std::nullopt);
    }
    ++num_field_updates_;
    refresh_field_updates_bytes();
    return {};
}

VoidResult BufferedUpdates::add_term(const Term &term, int32_t doc_id_upto) {
    auto current = delete_terms_.get(term);
    if (!current.has_value()) {
        return std::unexpected(std::move(current.error()));
    }
    if (current.value() != -1 && doc_id_upto < current.value()) {
        // Only a larger document count may replace the recorded one: when several
        // threads replace the same document, the one with the higher count has to
        // win regardless of the order they reach the buffer in.
        return {};
    }
    return delete_terms_.put(term, doc_id_upto);
}

VoidResult BufferedUpdates::add_query(std::shared_ptr<const Query> query, int32_t doc_id_upto) {
    if (query == nullptr) {
        return err(LuceneError::illegal_argument("a buffered delete needs a query"));
    }
    for (auto &[buffered, upto] : delete_queries_) {
        if (buffered != nullptr && buffered->equals(*query)) {
            upto = doc_id_upto;
            return {};
        }
    }
    delete_queries_.emplace_back(std::move(query), doc_id_upto);
    return {};
}

void BufferedUpdates::clear() {
    delete_terms_.clear();
    delete_queries_.clear();
    num_field_updates_ = 0;
    field_updates_.clear();
    bytes_used_ = 0;
    field_updates_bytes_used_ = 0;
}

bool BufferedUpdates::any() const {
    return delete_terms_.size() > 0 || !delete_queries_.empty() || num_field_updates_ > 0;
}

std::string BufferedUpdates::to_string() const {
    if (verbose_deletes_) {
        return std::format("gen={} deleteTerms={} deleteQueries={} fieldUpdates={} bytesUsed={}", gen_,
                           delete_terms_.to_string(), delete_queries_.size(), field_updates_.size(),
                           ram_bytes_used());
    }
    std::string out = std::format("gen={}", gen_);
    if (!delete_terms_.is_empty()) {
        out += std::format(" {} unique deleted terms", delete_terms_.size());
    }
    if (!delete_queries_.empty()) {
        out += std::format(" {} deleted queries", delete_queries_.size());
    }
    if (num_field_updates_ != 0) {
        out += std::format(" {} field updates", num_field_updates_);
    }
    if (const int64_t used = ram_bytes_used(); used != 0) {
        out += std::format(" bytesUsed={}", used);
    }
    return out;
}

} // namespace lucene
