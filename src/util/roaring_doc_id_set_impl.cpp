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

module lucene_core:roaring_doc_id_set.impl;

import :roaring_doc_id_set;

import std;

import :bits;
import :doc_id_set;
import :doc_id_set_impls;
import :doc_id_set_iterator;
import :error;
import :fixed_bit_set;
import :types;

namespace lucene {

namespace {

/// Number of documents per block.
constexpr size_t BLOCK_SIZE = size_t{1} << 16;
/// The longest array a block is encoded with; past that a bit set is used.
constexpr size_t MAX_ARRAY_LENGTH = size_t{1} << 12;

int32_t block_base(size_t block) { return static_cast<int32_t>(block << 16); }

/// Widens a stored 16 bit id back to the document id space.
int32_t widen(uint16_t id) { return static_cast<int32_t>(id); }

} // namespace

// ---------------------------------------------------------------------------
// ShortArrayDISI
// ---------------------------------------------------------------------------

Result<int32_t> ShortArrayDISI::next_doc() {
    const size_t i = i_.has_value() ? i_.value() + 1 : 0;
    i_ = i;
    if (i >= doc_ids_->size()) {
        doc_ = NO_MORE_DOCS;
        return doc_;
    }
    doc_ = widen((*doc_ids_)[i]);
    return doc_;
}

Result<int32_t> ShortArrayDISI::advance(int32_t target) {
    size_t lo = i_.has_value() ? i_.value() + 1 : 0;
    size_t hi = doc_ids_->size();
    while (lo < hi) {
        // Preserve the inclusive-range midpoint with an exclusive upper bound.
        const size_t mid = lo + ((hi - lo - 1) >> 1);
        const int32_t mid_doc = widen((*doc_ids_)[mid]);
        if (mid_doc < target) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == doc_ids_->size()) {
        i_ = doc_ids_->size();
        doc_ = NO_MORE_DOCS;
    } else {
        i_ = lo;
        doc_ = widen((*doc_ids_)[lo]);
    }
    return doc_;
}

Result<int64_t> ShortArrayDISI::cost() const { return static_cast<int64_t>(doc_ids_->size()); }

// ---------------------------------------------------------------------------
// ShortArrayDocIdSet
// ---------------------------------------------------------------------------

Result<std::unique_ptr<DocIdSetIterator>> ShortArrayDocIdSet::iterator() const {
    return std::unique_ptr<DocIdSetIterator>(
            new ShortArrayDISI(std::make_shared<const std::vector<uint16_t>>(doc_ids_)));
}

// ---------------------------------------------------------------------------
// RoaringDocIdSetIterator
// ---------------------------------------------------------------------------

Result<int32_t> RoaringDocIdSetIterator::first_doc_from_next_block() {
    while (true) {
        const size_t block = block_.has_value() ? block_.value() + 1 : 0;
        block_ = block;
        if (block >= sets_->size()) {
            doc_ = NO_MORE_DOCS;
            break;
        }
        if ((*sets_)[block] != nullptr) {
            auto opened = (*sets_)[block]->iterator();
            if (!opened.has_value()) {
                return std::unexpected(std::move(opened.error()));
            }
            sub_ = std::move(opened.value());
            auto sub_next = sub_->next_doc();
            if (!sub_next.has_value()) {
                return std::unexpected(std::move(sub_next.error()));
            }
            doc_ = block_base(block) | sub_next.value();
            break;
        }
    }
    return doc_;
}

Result<int32_t> RoaringDocIdSetIterator::next_doc() {
    if (sub_ == nullptr) {
        return first_doc_from_next_block();
    }
    auto sub_next = sub_->next_doc();
    if (!sub_next.has_value()) {
        return std::unexpected(std::move(sub_next.error()));
    }
    if (sub_next.value() == NO_MORE_DOCS) {
        return first_doc_from_next_block();
    }
    doc_ = block_base(block_.value()) | sub_next.value();
    return doc_;
}

Result<int32_t> RoaringDocIdSetIterator::advance(int32_t target) {
    const size_t target_block = static_cast<size_t>(target >> 16);
    if (!block_.has_value() || block_.value() != target_block) {
        block_ = target_block;
        if (target_block >= sets_->size()) {
            doc_ = NO_MORE_DOCS;
            return doc_;
        }
        const std::shared_ptr<DocIdSet> &slot = (*sets_)[target_block];
        if (slot == nullptr) {
            return first_doc_from_next_block();
        }
        auto opened = slot->iterator();
        if (!opened.has_value()) {
            return std::unexpected(std::move(opened.error()));
        }
        sub_ = std::move(opened.value());
    }
    if (sub_ == nullptr) {
        // The first block was empty; `first_doc_from_next_block` already moved on.
        return doc_;
    }
    auto sub_next = sub_->advance(target & 0xFFFF);
    if (!sub_next.has_value()) {
        return std::unexpected(std::move(sub_next.error()));
    }
    if (sub_next.value() == NO_MORE_DOCS) {
        return first_doc_from_next_block();
    }
    doc_ = block_base(block_.value()) | sub_next.value();
    return doc_;
}

// ---------------------------------------------------------------------------
// RoaringDocIdSet
// ---------------------------------------------------------------------------

Result<std::unique_ptr<DocIdSetIterator>> RoaringDocIdSet::iterator() const {
    return std::unique_ptr<DocIdSetIterator>(
            new RoaringDocIdSetIterator(sets_, static_cast<int64_t>(cardinality_)));
}

int64_t RoaringDocIdSet::ram_bytes_used() const {
    int64_t total = static_cast<int64_t>(sets_->capacity() * sizeof(std::shared_ptr<DocIdSet>));
    for (const std::shared_ptr<DocIdSet> &set : *sets_) {
        if (set == nullptr) {
            continue;
        }
        if (const auto *accountable = dynamic_cast<const Accountable *>(set.get()); accountable != nullptr) {
            total += accountable->ram_bytes_used();
        }
    }
    return total;
}

// ---------------------------------------------------------------------------
// RoaringDocIdSet::Builder
// ---------------------------------------------------------------------------

RoaringDocIdSet::Builder::Builder(size_t max_doc) : max_doc_(max_doc) {
    const size_t sets_length = (max_doc + BLOCK_SIZE - 1) >> 16;
    sets_.assign(sets_length, nullptr);
    buffer_.reserve(MAX_ARRAY_LENGTH);
}

VoidResult RoaringDocIdSet::Builder::add(int32_t doc_id) {
    if (doc_id <= last_doc_id_) {
        return err(LuceneError::illegal_argument(
                "doc ids must be added in order, got " + std::to_string(doc_id) +
                " which is <= the last one " + std::to_string(last_doc_id_)));
    }
    const size_t block = static_cast<size_t>(doc_id >> 16);
    if (!current_block_.has_value() || current_block_.value() != block) {
        if (auto flushed = flush(); !flushed.has_value()) {
            return flushed;
        }
        current_block_ = block;
    }

    if (current_block_cardinality_ < MAX_ARRAY_LENGTH) {
        buffer_.push_back(static_cast<uint16_t>(doc_id & 0xFFFF));
    } else {
        if (!dense_buffer_.has_value()) {
            // The buffer is full: move to a bit set, replaying what it holds.
            const size_t block_end = block << 16;
            const size_t num_bits = std::min(BLOCK_SIZE, max_doc_ > block_end ? max_doc_ - block_end : BLOCK_SIZE);
            dense_buffer_.emplace(num_bits);
            for (uint16_t buffered : buffer_) {
                dense_buffer_->set(buffered);
            }
        }
        dense_buffer_->set(static_cast<size_t>(doc_id) & 0xFFFF);
    }

    last_doc_id_ = doc_id;
    ++current_block_cardinality_;
    return {};
}

VoidResult RoaringDocIdSet::Builder::add_disi(DocIdSetIterator &disi) {
    auto doc = disi.next_doc();
    while (doc.has_value() && doc.value() != NO_MORE_DOCS) {
        if (auto added = add(doc.value()); !added.has_value()) {
            return added;
        }
        doc = disi.next_doc();
    }
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    return {};
}

Result<std::shared_ptr<RoaringDocIdSet>> RoaringDocIdSet::Builder::build() {
    if (auto flushed = flush(); !flushed.has_value()) {
        return std::unexpected(std::move(flushed.error()));
    }
    return std::shared_ptr<RoaringDocIdSet>(new RoaringDocIdSet(
            std::make_shared<const std::vector<std::shared_ptr<DocIdSet>>>(std::move(sets_)), cardinality_));
}

VoidResult RoaringDocIdSet::Builder::flush() {
    if (current_block_cardinality_ <= MAX_ARRAY_LENGTH) {
        // Sparse encoding: keep the plain array.
        if (current_block_cardinality_ > 0) {
            if (!current_block_.has_value()) {
                return err(LuceneError::illegal_state("a non-empty roaring block has no current block"));
            }
            sets_[current_block_.value()] = std::make_shared<ShortArrayDocIdSet>(std::move(buffer_));
            buffer_ = {};
            buffer_.reserve(MAX_ARRAY_LENGTH);
        }
    } else {
        if (!dense_buffer_.has_value()) {
            return err(LuceneError::illegal_state("a dense roaring block has no bit set"));
        }
        if (!current_block_.has_value()) {
            return err(LuceneError::illegal_state("a non-empty roaring block has no current block"));
        }
        const size_t block = current_block_.value();
        if (dense_buffer_->length() == BLOCK_SIZE &&
            BLOCK_SIZE - current_block_cardinality_ < MAX_ARRAY_LENGTH) {
            // Dense encoding: store the documents that are *not* in the set.
            std::vector<uint16_t> excluded;
            excluded.reserve(BLOCK_SIZE - current_block_cardinality_);
            for (size_t doc = 0; doc < BLOCK_SIZE; ++doc) {
                if (!dense_buffer_->get_bit(doc)) {
                    excluded.push_back(static_cast<uint16_t>(doc));
                }
            }
            sets_[block] = std::make_shared<NotDocIdSet>(
                    static_cast<int32_t>(BLOCK_SIZE),
                    std::make_shared<ShortArrayDocIdSet>(std::move(excluded)));
        } else {
            auto bits = std::make_shared<FixedBitSet>(std::move(dense_buffer_.value()));
            dense_buffer_.reset();
            auto medium = BitDocIdSet::create_with_cost(bits, static_cast<int64_t>(current_block_cardinality_));
            if (!medium.has_value()) {
                return std::unexpected(std::move(medium.error()));
            }
            sets_[block] = std::move(medium.value());
        }
        buffer_.clear();
    }
    cardinality_ += current_block_cardinality_;
    dense_buffer_.reset();
    current_block_cardinality_ = 0;
    return {};
}

} // namespace lucene
