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

module lucene_core:doc_id_set_impls.impl;

import :doc_id_set_impls;
import :types;
import std;

import :bits;
import :doc_id_set;
import :doc_id_set_iterator;
import :error;
import :fixed_bit_set;

namespace lucene {

// ---------------------------------------------------------------------------
// IntArrayDocIdSet
// ---------------------------------------------------------------------------

Result<std::shared_ptr<IntArrayDocIdSet>> IntArrayDocIdSet::create(std::vector<int32_t> docs, size_t length) {
    if (length >= docs.size()) {
        return err(LuceneError::array_index_out_of_bounds(
                std::format("length {} is out of bounds for docs of length {}", length, docs.size())));
    }
    if (docs[length] != NO_MORE_DOCS) {
        return err(LuceneError::illegal_argument(std::format("docs[{}] must be NO_MORE_DOCS", length)));
    }
    for (size_t i = 1; i < length; ++i) {
        if (docs[i - 1] > docs[i]) {
            return err(LuceneError::illegal_argument(std::format("docs must be sorted, but docs[{}] > docs[{}]", i - 1, i)));
        }
    }
    auto shared = std::make_shared<const std::vector<int32_t>>(std::move(docs));
    return std::shared_ptr<IntArrayDocIdSet>(new IntArrayDocIdSet(std::move(shared), length));
}

Result<std::unique_ptr<DocIdSetIterator>> IntArrayDocIdSet::iterator() const {
    std::unique_ptr<DocIdSetIterator> it(new IntArrayDocIdSetIterator(docs_, length_));
    return it;
}

Result<int32_t> IntArrayDocIdSetIterator::next_doc() {
    // **Deviation:** the reference implementation indexes past the array when
    // [`next_doc`] is called again after it returned NO_MORE_DOCS; here the
    // iterator simply stays exhausted.
    if (i_ >= length_) {
        doc_ = NO_MORE_DOCS;
        return doc_;
    }
    doc_ = (*docs_)[i_];
    i_ += 1;
    return doc_;
}

Result<int32_t> IntArrayDocIdSetIterator::advance(int32_t target) {
    if (i_ >= length_) {
        doc_ = NO_MORE_DOCS;
        return doc_;
    }
    // `docs_[length_]` is NO_MORE_DOCS, so galloping cannot run off the end.
    size_t bound = 1;
    while (i_ + bound < length_ && (*docs_)[i_ + bound] < target) {
        bound *= 2;
    }
    size_t start = i_ + bound / 2;
    const size_t end = std::min(i_ + bound + 1, length_);
    const auto begin = docs_->begin() + static_cast<std::ptrdiff_t>(start);
    const auto last = docs_->begin() + static_cast<std::ptrdiff_t>(end);
    const auto found = std::lower_bound(begin, last, target);
    start += static_cast<size_t>(found - begin);
    doc_ = (*docs_)[start];
    i_ = start + 1;
    return doc_;
}

// ---------------------------------------------------------------------------
// NotDocIdSet
// ---------------------------------------------------------------------------

Result<bool> NotDocIdBits::get(size_t index) const {
    auto value = in_->get(index);
    if (!value) {
        return std::unexpected(std::move(value.error()));
    }
    return !value.value();
}

Result<std::unique_ptr<DocIdSetIterator>> NotDocIdSet::iterator() const {
    auto in = in_->iterator();
    if (!in) {
        return std::unexpected(std::move(in.error()));
    }
    std::unique_ptr<DocIdSetIterator> it(new NotDocDocIdSetIterator(std::move(in.value()), max_doc_));
    return it;
}

Result<std::shared_ptr<const Bits>> NotDocIdSet::bits() const {
    auto in = in_->bits();
    if (!in) {
        return std::unexpected(std::move(in.error()));
    }
    if (in.value() == nullptr) {
        return std::shared_ptr<const Bits>{};
    }
    return std::shared_ptr<const Bits>(std::make_shared<NotDocIdBits>(in.value()));
}

Result<int32_t> NotDocDocIdSetIterator::advance(int32_t target) {
    doc_ = target;
    if (doc_ > next_skipped_doc_) {
        auto skipped = in_->advance(doc_);
        if (!skipped) {
            return std::unexpected(std::move(skipped.error()));
        }
        next_skipped_doc_ = skipped.value();
    }
    while (true) {
        if (doc_ >= max_doc_) {
            doc_ = NO_MORE_DOCS;
            break;
        }
        if (doc_ != next_skipped_doc_) {
            return doc_;
        }
        doc_ += 1;
        auto skipped = in_->next_doc();
        if (!skipped) {
            return std::unexpected(std::move(skipped.error()));
        }
        next_skipped_doc_ = skipped.value();
    }
    return doc_;
}

// ---------------------------------------------------------------------------
// BitDocIdSet
// ---------------------------------------------------------------------------

Result<std::shared_ptr<BitDocIdSet>> BitDocIdSet::create_with_cost(std::shared_ptr<const Bits> set, int64_t cost) {
    if (cost < 0) {
        return err(LuceneError::illegal_argument(std::format("cost must be >= 0, got {}", cost)));
    }
    if (set == nullptr) {
        return err(LuceneError::illegal_argument("set must not be null"));
    }
    return std::shared_ptr<BitDocIdSet>(new BitDocIdSet(std::move(set), cost));
}

Result<std::shared_ptr<BitDocIdSet>> BitDocIdSet::create(std::shared_ptr<const Bits> set) {
    if (set == nullptr) {
        return err(LuceneError::illegal_argument("set must not be null"));
    }
    int64_t cost = static_cast<int64_t>(set->length());
    if (const auto *fixed = dynamic_cast<const FixedBitSet *>(set.get()); fixed != nullptr) {
        cost = static_cast<int64_t>(fixed->cardinality());
    }
    return create_with_cost(std::move(set), cost);
}

Result<std::unique_ptr<DocIdSetIterator>> BitDocIdSet::iterator() const {
    std::unique_ptr<DocIdSetIterator> it(new BitSetIterator(set_, cost_));
    return it;
}

Result<int32_t> BitSetIterator::advance(int32_t target) {
    const auto length = static_cast<int32_t>(bits_->length());
    for (int32_t doc = target < 0 ? 0 : target; doc < length; ++doc) {
        auto value = bits_->get(static_cast<size_t>(doc));
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        if (value.value()) {
            doc_ = doc;
            return doc_;
        }
    }
    doc_ = NO_MORE_DOCS;
    return doc_;
}


namespace {

constexpr int64_t kDocIdSetOverheadBytes = 64;

} // namespace

int64_t IntArrayDocIdSet::ram_bytes_used() const {
    return static_cast<int64_t>(docs_->capacity() * sizeof(int32_t)) + kDocIdSetOverheadBytes;
}

int64_t BitDocIdSet::ram_bytes_used() const {
    if (const auto *fixed = dynamic_cast<const FixedBitSet *>(set_.get()); fixed != nullptr) {
        return fixed->ram_bytes_used() + kDocIdSetOverheadBytes;
    }
    return kDocIdSetOverheadBytes;
}

int64_t NotDocIdSet::ram_bytes_used() const {
    int64_t inner = 0;
    if (const auto *accountable = dynamic_cast<const Accountable *>(in_.get()); accountable != nullptr) {
        inner = accountable->ram_bytes_used();
    }
    return kDocIdSetOverheadBytes + inner;
}

} // namespace lucene
