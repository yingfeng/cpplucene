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

module lucene_core:bytes_ref_hash.impl;

import :bytes_ref_hash;

import :array_util;
import :bit_util;
import :byte_block_pool;
import :bytes_ref;
import :bytes_ref_block_pool;
import :bytes_ref_comparator;
import :debug;
import :error;
import :string_sorter;
import :types;

import std;

namespace lucene {

namespace {

/// Sorts a [`BytesRefHash`]'s id array by term bytes.
///
/// Port of `StringSorterImpl` (`util/bytes_ref_hash.rs`), reduced to the
/// `StringSorterBase` half: the byte-at access goes through the pool instead of
/// a per-bucket cache, so `reorder`/`build_histogram`/`should_fallback` stay at
/// their defaults.  The order produced is identical.
class HashStringSorter final : public StringSorterBase {
public:
    HashStringSorter(std::vector<int32_t> &ids, const BytesRefHash &hash, const ByteBlockPool &byte_block_pool)
            : ids_(ids), hash_(hash), byte_block_pool_(byte_block_pool) {}

    [[nodiscard]] Result<BytesRef> get(size_t i) override {
        auto start = hash_.byte_start(ids_.at(i));
        if (!start.has_value()) {
            return std::unexpected(std::move(start.error()));
        }
        const auto position = pool_.fill_bytes_ref(start.value(), byte_block_pool_);
        return pool_.bytes_ref(position, byte_block_pool_);
    }

    [[nodiscard]] VoidResult swap(size_t i, size_t j) override {
        std::swap(ids_.at(i), ids_.at(j));
        return {};
    }

private:
    std::vector<int32_t> &ids_;
    const BytesRefHash &hash_;
    const ByteBlockPool &byte_block_pool_;
    /// A stateless helper: the term bytes live in `byte_block_pool_`.
    BytesRefBlockPool pool_{};
};

} // namespace

// ---------------------------------------------------------------------------
// DirectBytesStartArray
// ---------------------------------------------------------------------------

VoidResult DirectBytesStartArray::init() {
    auto size = ArrayUtil::oversize(init_size_, BitUtil::INT_BYTES);
    if (!size.has_value()) {
        return std::unexpected(std::move(size.error()));
    }
    bytes_start_.assign(size.value(), 0);
    init_ = true;
    return {};
}

VoidResult DirectBytesStartArray::grow() {
    debug_check(init_, "the table must be initialized before it grows");
    auto grown = ArrayUtil::grow_with(bytes_start_, bytes_start_.size() + 1, [] { return int32_t{0}; });
    if (!grown.has_value()) {
        return grown;
    }
    return {};
}

void DirectBytesStartArray::clear() {
    init_ = false;
    bytes_start_.clear();
}

Result<int32_t> DirectBytesStartArray::get_value(size_t index) const {
    if (index >= bytes_start_.size()) {
        return err(LuceneError::illegal_argument("bytes start index " + std::to_string(index) + " is out of bounds"));
    }
    return bytes_start_[index];
}

VoidResult DirectBytesStartArray::set_value(size_t index, int32_t value) {
    if (index >= bytes_start_.size()) {
        return err(LuceneError::illegal_argument("bytes start index " + std::to_string(index) + " is out of bounds"));
    }
    bytes_start_[index] = value;
    return {};
}

// ---------------------------------------------------------------------------
// BytesRefHash
// ---------------------------------------------------------------------------

BytesRefHash::BytesRefHash()
        : BytesRefHash(DEFAULT_CAPACITY, std::make_unique<DirectBytesStartArray>(DEFAULT_CAPACITY)) {}

BytesRefHash::BytesRefHash(size_t capacity, std::unique_ptr<BytesStartArray> bytes_start_array)
        : hash_size_(capacity),
          hash_half_size_(capacity >> 1),
          hash_mask_(capacity - 1),
          ids_(capacity, -1),
          bytes_start_array_(std::move(bytes_start_array)) {
    if (bytes_start_array_ != nullptr) {
        // Failure is reported lazily by `add`/`reinit`, which check `need_init`.
        (void)bytes_start_array_->init();
    }
}

Result<BytesRefBlockPoolPosition> BytesRefHash::get(int32_t bytes_id, const ByteBlockPool &pool) const {
    const auto index = static_cast<size_t>(bytes_id);
    debug_check(!bytes_start_array_->need_init(), "bytes_start is null - not initialized");
    auto len = bytes_start_array_->len();
    if (!len.has_value()) {
        return std::unexpected(std::move(len.error()));
    }
    if (index >= len.value()) {
        return err(LuceneError::illegal_argument("bytesID " + std::to_string(bytes_id) + " exceeds bytes_start len " +
                                                 std::to_string(len.value())));
    }
    auto value = bytes_start_array_->get_value(index);
    if (!value.has_value()) {
        return std::unexpected(std::move(value.error()));
    }
    return pool_.fill_bytes_ref(value.value(), pool);
}

Result<const std::vector<int32_t> *> BytesRefHash::compact() {
    debug_check(!bytes_start_array_->need_init(), "bytes_start is null - not initialized");

    size_t upto = 0;
    for (size_t i = 0; i < hash_size_; ++i) {
        if (ids_[i] != -1) {
            if (upto < i) {
                ids_[upto] = ids_[i];
                ids_[i] = -1;
            }
            upto += 1;
        }
    }
    debug_check(upto == count_, "compaction must visit exactly `count` entries");
    last_count_ = count_;
    return &ids_;
}

VoidResult BytesRefHash::sort(const ByteBlockPool &byte_block_pool) {
    if (count_ <= 1) {
        if (auto compacted = compact(); !compacted.has_value()) {
            return std::unexpected(std::move(compacted.error()));
        }
        return {};
    }
    auto compacted = compact();
    if (!compacted.has_value()) {
        return std::unexpected(std::move(compacted.error()));
    }

    NaturalBytesRefComparator comparator;
    StringSorter sorter(std::make_unique<HashStringSorter>(ids_, *this, byte_block_pool), comparator);
    if (auto sorted = sorter.sort(0, count_); !sorted.has_value()) {
        return sorted;
    }

    // The tail of the id array is unused again once the sort is done.
    for (size_t i = count_; i < ids_.size(); ++i) {
        ids_[i] = -1;
    }
    return {};
}

void BytesRefHash::clear_with_reset_pool(bool reset_pool, ByteBlockPool &byte_block_pool) {
    last_count_ = count_;
    count_ = 0;

    if (reset_pool) {
        pool_.reset(byte_block_pool);
    }

    bytes_start_array_->clear();

    if (last_count_.has_value() && shrink(last_count_.value())) {
        // `shrink` cleared the hash entries.
        return;
    }
    std::fill(ids_.begin(), ids_.end(), int32_t{-1});
}

void BytesRefHash::clear(ByteBlockPool &byte_block_pool) { clear_with_reset_pool(true, byte_block_pool); }

void BytesRefHash::close(ByteBlockPool &byte_block_pool) {
    clear_with_reset_pool(true, byte_block_pool);
    ids_.clear();
}

Result<int32_t> BytesRefHash::add(const BytesRef &bytes, ByteBlockPool &byte_block_pool) {
    debug_check(!bytes_start_array_->need_init(), "bytes_start is null - not initialized");

    auto hash_pos = find_hash(bytes, byte_block_pool);
    if (!hash_pos.has_value()) {
        return std::unexpected(std::move(hash_pos.error()));
    }
    int32_t e = ids_[hash_pos.value()];
    if (e == -1) {
        auto length = bytes_start_array_->len();
        if (!length.has_value()) {
            return std::unexpected(std::move(length.error()));
        }
        const size_t index = count_;
        if (index >= length.value()) {
            if (auto grown = bytes_start_array_->grow(); !grown.has_value()) {
                return std::unexpected(std::move(grown.error()));
            }
        }

        auto value = pool_.add_bytes_ref(bytes, byte_block_pool);
        if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
        }
        if (auto stored = bytes_start_array_->set_value(index, value.value()); !stored.has_value()) {
            return std::unexpected(std::move(stored.error()));
        }
        e = static_cast<int32_t>(count_);
        count_ += 1;
        debug_check(ids_[hash_pos.value()] == -1, "the slot must still be free");
        ids_[hash_pos.value()] = e;

        if (count_ == hash_half_size_) {
            if (auto rehashed = rehash(2 * hash_size_, true, byte_block_pool); !rehashed.has_value()) {
                return std::unexpected(std::move(rehashed.error()));
            }
        }
        return e;
    }
    return -(e + 1);
}

Result<int32_t> BytesRefHash::find(const BytesRef &bytes, const ByteBlockPool &byte_block_pool) const {
    auto hash_pos = find_hash(bytes, byte_block_pool);
    if (!hash_pos.has_value()) {
        return std::unexpected(std::move(hash_pos.error()));
    }
    return ids_[hash_pos.value()];
}

Result<size_t> BytesRefHash::find_hash(const BytesRef &bytes, const ByteBlockPool &pool) const {
    debug_check(!bytes_start_array_->need_init(), "bytes_start is null - not initialized");

    // `BytesRef::hash_code()` is the reference implementation's `do_hash(bytes, 0, length)` with the
    // same seed (and, for a single-byte term, the same lookup-table value).
    auto code_value = static_cast<uint32_t>(bytes.hash_code());
    size_t hash_pos = static_cast<size_t>(code_value) & hash_mask_;
    int32_t e = ids_[hash_pos];
    if (e != -1) {
        auto start = bytes_start_array_->get_value(static_cast<size_t>(e));
        if (!start.has_value()) {
            return std::unexpected(std::move(start.error()));
        }
        if (!pool_.equals(start.value(), bytes, pool)) {
            while (true) {
                code_value = code_value + 1;
                hash_pos = static_cast<size_t>(code_value) & hash_mask_;
                e = ids_[hash_pos];
                if (e == -1) {
                    break;
                }
                auto probe = bytes_start_array_->get_value(static_cast<size_t>(e));
                if (!probe.has_value()) {
                    return std::unexpected(std::move(probe.error()));
                }
                if (pool_.equals(probe.value(), bytes, pool)) {
                    break;
                }
            }
        }
    }
    return hash_pos;
}

Result<int32_t> BytesRefHash::add_by_pool_offset(int32_t offset, ByteBlockPool &byte_block_pool) {
    debug_check(!bytes_start_array_->need_init(), "bytes_start is null - not initialized");

    int32_t code = offset;
    size_t hash_pos = static_cast<size_t>(static_cast<uint32_t>(offset)) & hash_mask_;
    int32_t e = ids_[hash_pos];
    auto length = bytes_start_array_->len();
    if (!length.has_value()) {
        return std::unexpected(std::move(length.error()));
    }
    while (e != -1) {
        auto value = bytes_start_array_->get_value(static_cast<size_t>(e));
        if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
        }
        if (value.value() == offset) {
            break;
        }
        code = code + 1;
        hash_pos = static_cast<size_t>(static_cast<uint32_t>(code)) & hash_mask_;
        e = ids_[hash_pos];
    }

    if (e == -1) {
        const size_t index = count_;
        if (index >= length.value()) {
            if (auto grown = bytes_start_array_->grow(); !grown.has_value()) {
                return std::unexpected(std::move(grown.error()));
            }
        }

        e = static_cast<int32_t>(count_);
        count_ += 1;
        if (auto stored = bytes_start_array_->set_value(index, offset); !stored.has_value()) {
            return std::unexpected(std::move(stored.error()));
        }

        debug_check(ids_[hash_pos] == -1, "the slot must still be free");
        ids_[hash_pos] = e;

        if (count_ == hash_half_size_) {
            if (auto rehashed = rehash(2 * hash_size_, false, byte_block_pool); !rehashed.has_value()) {
                return std::unexpected(std::move(rehashed.error()));
            }
        }
        return e;
    }
    return -(e + 1);
}

VoidResult BytesRefHash::rehash(size_t new_size, bool hash_on_data, ByteBlockPool &byte_block_pool) {
    const size_t new_mask = new_size - 1;
    std::vector<int32_t> new_hash(new_size, -1);
    for (size_t i = 0; i < hash_size_; ++i) {
        const int32_t e0 = ids_[i];
        if (e0 == -1) {
            continue;
        }
        auto start = bytes_start_array_->get_value(static_cast<size_t>(e0));
        if (!start.has_value()) {
            return std::unexpected(std::move(start.error()));
        }
        uint32_t code = hash_on_data ? static_cast<uint32_t>(pool_.hash(start.value(), byte_block_pool))
                                     : static_cast<uint32_t>(start.value());

        size_t hash_pos = static_cast<size_t>(code) & new_mask;
        if (new_hash[hash_pos] != -1) {
            while (true) {
                code = code + 1;
                hash_pos = static_cast<size_t>(code) & new_mask;
                if (new_hash[hash_pos] == -1) {
                    break;
                }
            }
        }
        new_hash[hash_pos] = e0;
    }
    hash_mask_ = new_mask;
    ids_ = std::move(new_hash);
    hash_size_ = new_size;
    hash_half_size_ = new_size / 2;
    return {};
}

bool BytesRefHash::shrink(size_t target_size) {
    // Cannot use ArrayUtil::shrink here because the size must stay a power of 2.
    size_t new_size = hash_size_;
    while (new_size >= 8 && new_size / 4 > target_size) {
        new_size /= 2;
    }

    if (new_size != hash_size_) {
        hash_size_ = new_size;
        ids_.assign(hash_size_, -1);
        hash_half_size_ = new_size / 2;
        hash_mask_ = new_size - 1;
        return true;
    }
    return false;
}

VoidResult BytesRefHash::reinit() {
    if (bytes_start_array_->need_init()) {
        if (auto initialized = bytes_start_array_->init(); !initialized.has_value()) {
            return initialized;
        }
    }

    if (ids_.empty()) {
        ids_.assign(hash_size_, -1);
    }
    return {};
}

Result<int32_t> BytesRefHash::byte_start(int32_t bytes_id) const {
    debug_check(!bytes_start_array_->need_init(), "bytes_start is null - not initialized");
    debug_check(bytes_id >= 0 && static_cast<size_t>(bytes_id) < count_, "bytes id is out of range");
    return bytes_start_array_->get_value(static_cast<size_t>(bytes_id));
}

int64_t BytesRefHash::ram_bytes_used() const {
    int64_t bytes = static_cast<int64_t>(ids_.size() * sizeof(int32_t));
    if (const auto *direct = dynamic_cast<const DirectBytesStartArray *>(bytes_start_array_.get());
        direct != nullptr) {
        bytes += static_cast<int64_t>(direct->ram_bytes());
    }
    return bytes;
}

} // namespace lucene
