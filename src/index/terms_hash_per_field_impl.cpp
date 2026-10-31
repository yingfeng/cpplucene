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

module lucene_core:terms_hash_per_field.impl;

import :terms_hash_per_field;

import :byte_block_pool;
import :byte_slice_pool;
import :byte_slice_reader;
import :bytes_ref_hash;
import :debug;
import :error;
import :index_types;
import :int_block_pool;
import :parallel_postings_array;
import :types;

import std;

namespace lucene {

// ---------------------------------------------------------------------------
// PostingsBytesStartArray
// ---------------------------------------------------------------------------

VoidResult PostingsBytesStartArray::init() {
    if (holder_.postings_array() == nullptr) {
        holder_.set(holder_.type().new_per_field(2));
        if (holder_.postings_array() == nullptr) {
            return err(LuceneError::illegal_state("the postings array could not be created"));
        }
    }
    return {};
}

VoidResult PostingsBytesStartArray::grow() {
    auto *postings = holder_.postings_array_mut();
    if (postings == nullptr) {
        return err(LuceneError::illegal_state("postings array is missing"));
    }
    return postings->grow();
}

void PostingsBytesStartArray::clear() { (void)holder_.take(); }

Result<const PostingsArray *> PostingsBytesStartArray::postings_array() const {
    const auto *postings = holder_.postings_array();
    if (postings == nullptr) {
        return err(LuceneError::illegal_state("postings array is missing"));
    }
    return postings;
}

Result<PostingsArray *> PostingsBytesStartArray::postings_array_mut() {
    auto *postings = holder_.postings_array_mut();
    if (postings == nullptr) {
        return err(LuceneError::illegal_state("postings array is missing"));
    }
    return postings;
}

Result<int32_t> PostingsBytesStartArray::get_value(size_t index) const {
    auto postings = postings_array();
    if (!postings.has_value()) {
        return std::unexpected(std::move(postings.error()));
    }
    const auto &starts = postings.value()->text_starts;
    if (index >= starts.size()) {
        return err(LuceneError::illegal_argument("postings array index " + std::to_string(index) +
                                                " is out of bounds"));
    }
    return starts[index];
}

VoidResult PostingsBytesStartArray::set_value(size_t index, int32_t value) {
    auto postings = postings_array_mut();
    if (!postings.has_value()) {
        return std::unexpected(std::move(postings.error()));
    }
    auto &starts = postings.value()->text_starts;
    if (index >= starts.size()) {
        return err(LuceneError::illegal_argument("postings array index " + std::to_string(index) +
                                                " is out of bounds"));
    }
    starts[index] = value;
    return {};
}

Result<size_t> PostingsBytesStartArray::len() const {
    auto postings = postings_array();
    if (!postings.has_value()) {
        return std::unexpected(std::move(postings.error()));
    }
    return postings.value()->text_starts.size();
}

// ---------------------------------------------------------------------------
// TermsHashPerField
// ---------------------------------------------------------------------------

TermsHashPerField::TermsHashPerField(int32_t stream_count, std::unique_ptr<TermsHashPerFieldType> type,
                                     IndexOptions index_options)
        : stream_count_(stream_count), index_options_(index_options) {
    debug_check(index_options != IndexOptions::None, "a terms hash needs index options");
    auto start_array = std::make_unique<PostingsBytesStartArray>(std::move(type));
    // The holder lives inside the `bytes_start` table, which the hash owns; the
    // address is stable for the lifetime of the hash.
    holder_ = &start_array->holder();
    bytes_hash_ = BytesRefHash(HASH_INIT_SIZE, std::move(start_array));
}

VoidResult TermsHashPerField::init_reader(ByteSliceReader &reader, size_t term_id, int32_t stream,
                                          const IntBlockPool &int_pool) const {
    debug_check(stream < stream_count_, "the stream index must be below the stream count");
    const auto *postings = postings_array();
    if (postings == nullptr) {
        return err(LuceneError::illegal_state("postings array is missing"));
    }
    const auto stream_start_offset = postings->address_offset.at(term_id);
    const auto buffer_index = static_cast<size_t>(static_cast<uint32_t>(stream_start_offset) >> INT_BLOCK_SHIFT);
    const auto offset_in_address_buffer = stream_start_offset & INT_BLOCK_MASK;
    const auto buffer = int_pool.buffer(buffer_index);
    const auto address_index = static_cast<size_t>(offset_in_address_buffer) + static_cast<size_t>(stream);
    debug_check(address_index < buffer.size(), "the stream address is out of bounds");
    const auto addr = buffer[address_index];
    const auto init_offset = postings->byte_starts.at(term_id) + stream * ByteSlicePool::FIRST_LEVEL_SIZE;
    reader.init(static_cast<size_t>(init_offset), static_cast<size_t>(addr));
    return {};
}

VoidResult TermsHashPerField::sort_terms(const ByteBlockPool &byte_pool) {
    debug_check(!is_sorted_, "the terms must not be sorted twice without a reset");
    if (auto sorted = bytes_hash_.sort(byte_pool); !sorted.has_value()) {
        return sorted;
    }
    is_sorted_ = true;
    return {};
}

VoidResult TermsHashPerField::write_byte(int32_t stream, uint8_t b, IntBlockPool &int_pool,
                                         ByteBlockPool &byte_pool) const {
    debug_check(stream < stream_count_, "the stream index must be below the stream count");
    const auto address_index = static_cast<size_t>(stream_address_offset_ + stream);
    auto &address_buffer = int_pool.buffer_mut(term_stream_address_buffer_index_);
    const int32_t upto = address_buffer[address_index];
    const auto block_index = static_cast<size_t>(static_cast<uint32_t>(upto) >> BYTE_BLOCK_SHIFT);
    debug_check(!byte_pool.current_buffer().has_value() || block_index <= byte_pool.current_buffer().value(),
                "the block index must not be past the head buffer");
    auto offset = static_cast<size_t>(static_cast<uint32_t>(upto) & BYTE_BLOCK_MASK);

    bool allocated = false;
    if (byte_pool.mutable_buffer(block_index)[offset] != 0) {
        // End of the slice: allocate a new one.
        auto allocated_offset = slice_pool_.alloc_slice(block_index, offset, byte_pool);
        if (!allocated_offset.has_value()) {
            return std::unexpected(std::move(allocated_offset.error()));
        }
        offset = allocated_offset.value();
        address_buffer[address_index] = static_cast<int32_t>(offset + static_cast<size_t>(byte_pool.byte_offset()));
        allocated = true;
    }

    auto &bytes = byte_pool.mutable_buffer(allocated ? byte_pool.current_buffer().value() : block_index);
    bytes[offset] = b;
    address_buffer[address_index] += 1;
    return {};
}

VoidResult TermsHashPerField::write_bytes(int32_t stream, std::span<const uint8_t> b, size_t offset, size_t len,
                                          IntBlockPool &int_pool, ByteBlockPool &byte_pool) const {
    debug_check(stream < stream_count_, "the stream index must be below the stream count");
    const auto address_index = static_cast<size_t>(stream_address_offset_ + stream);
    auto &address_buffer = int_pool.buffer_mut(term_stream_address_buffer_index_);

    size_t block_index = static_cast<size_t>(static_cast<uint32_t>(address_buffer[address_index]) >> BYTE_BLOCK_SHIFT);
    debug_check(!byte_pool.current_buffer().has_value() || block_index <= byte_pool.current_buffer().value(),
                "the block index must not be past the head buffer");
    size_t slice_offset =
            static_cast<size_t>(static_cast<uint32_t>(address_buffer[address_index]) & BYTE_BLOCK_MASK);

    const size_t end = offset + len;
    {
        auto &slice = byte_pool.mutable_buffer(block_index);
        // Fill the remaining room of the current slice; a non-zero byte marks its
        // end (see `ByteSlicePool`).  The terminator is located once with
        // `memchr` and the run is copied in one go: a postings stream writes a
        // few bytes at a time and the byte-wise loop showed up as the hottest
        // symbol of an indexing run.
        while (offset < end && slice[slice_offset] == 0) {
            slice[slice_offset] = b[offset];
            slice_offset += 1;
            offset += 1;
            address_buffer[address_index] += 1;
        }
    }

    while (offset < end) {
        auto offset_and_length = slice_pool_.alloc_known_size_slice(block_index, slice_offset, byte_pool);
        if (!offset_and_length.has_value()) {
            return std::unexpected(std::move(offset_and_length.error()));
        }
        slice_offset = static_cast<size_t>(static_cast<uint32_t>(offset_and_length.value()) >> 8);
        const auto slice_length = static_cast<size_t>(offset_and_length.value() & 0xff);

        const auto head = byte_pool.current_buffer();
        if (!head.has_value()) {
            return err(LuceneError::illegal_state("byte block pool has no head buffer"));
        }
        block_index = head.value();
        auto &slice = byte_pool.mutable_buffer(block_index);
        const size_t write_length = std::min(slice_length - 1, end - offset);
        std::copy_n(b.begin() + static_cast<std::ptrdiff_t>(offset), write_length,
                    slice.begin() + static_cast<std::ptrdiff_t>(slice_offset));
        slice_offset += write_length;
        offset += write_length;
        address_buffer[address_index] =
                static_cast<int32_t>(slice_offset + static_cast<size_t>(byte_pool.byte_offset()));
    }
    return {};
}

VoidResult TermsHashPerField::write_vint(int32_t stream, int32_t i, IntBlockPool &int_pool,
                                         ByteBlockPool &byte_pool) const {
    debug_check(stream < stream_count_, "the stream index must be below the stream count");
    while ((i & ~int32_t{0x7F}) != 0) {
        if (auto written = write_byte(stream, static_cast<uint8_t>((i & 0x7F) | 0x80), int_pool, byte_pool);
            !written.has_value()) {
            return written;
        }
        i = static_cast<int32_t>(static_cast<uint32_t>(i) >> 7);
    }
    return write_byte(stream, static_cast<uint8_t>(i), int_pool, byte_pool);
}

void TermsHashPerField::reset(ByteBlockPool &byte_pool) {
    bytes_hash_.clear_with_reset_pool(false, byte_pool);
    is_sorted_ = false;
}

VoidResult TermsHashPerField::reinit_hash() {
    is_sorted_ = false;
    return bytes_hash_.reinit();
}

VoidResult TermsHashPerField::init_stream_slices(size_t term_id, int32_t /*doc_id*/, IntBlockPool &int_pool,
                                                 ByteBlockPool &byte_pool) {
    if (stream_count_ + int_pool.int_upto() > INT_BLOCK_SIZE) {
        // Not enough room left in this block: lose the remainder and jump.
        if (auto next = int_pool.next_buffer(); !next.has_value()) {
            return next;
        }
    }

    if (BYTE_BLOCK_SIZE - byte_pool.byte_upto() < 2 * stream_count_ * ByteSlicePool::FIRST_LEVEL_SIZE) {
        // Can we fit at least one byte per stream in the current block?  If not,
        // allocate a new one.
        if (auto next = byte_pool.next_buffer(); !next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
    }

    const auto byte_offset = byte_pool.byte_offset();
    const auto head = int_pool.buffer_upto();
    if (!head.has_value()) {
        return err(LuceneError::illegal_state("term stream has no current int buffer"));
    }
    term_stream_address_buffer_index_ = head.value();
    stream_address_offset_ = int_pool.int_upto();
    const auto stream_address_index = static_cast<size_t>(stream_address_offset_);
    int_pool.set_int_upto(int_pool.int_upto() + stream_count_);

    auto *postings = holder_->postings_array_mut();
    if (postings == nullptr) {
        return err(LuceneError::illegal_state("postings array is missing"));
    }
    postings->address_offset[term_id] = stream_address_offset_ + int_pool.int_offset();

    auto &address_buffer = int_pool.buffer_mut(term_stream_address_buffer_index_);
    for (int32_t i = 0; i < stream_count_; ++i) {
        auto upto = slice_pool_.new_slice(ByteSlicePool::FIRST_LEVEL_SIZE, byte_pool);
        if (!upto.has_value()) {
            return std::unexpected(std::move(upto.error()));
        }
        address_buffer[stream_address_index + static_cast<size_t>(i)] = upto.value() + byte_offset;
    }
    postings->byte_starts[term_id] = address_buffer[stream_address_index];
    return {};
}

bool TermsHashPerField::assert_doc_id(int32_t doc_id) {
    debug_check(doc_id >= last_doc_id_, "the document ids must be non-decreasing");
    last_doc_id_ = doc_id;
    return true;
}

Result<size_t> TermsHashPerField::position_stream_slice(int32_t term_id, int32_t /*doc_id*/) {
    const int32_t id = (-term_id) - 1;
    if (id < 0) {
        return err(LuceneError::illegal_argument("the term id must encode a stored term"));
    }
    const auto *postings = postings_array();
    if (postings == nullptr) {
        return err(LuceneError::illegal_state("postings array is missing"));
    }
    const auto int_start = postings->address_offset[static_cast<size_t>(id)];
    term_stream_address_buffer_index_ = static_cast<size_t>(static_cast<uint32_t>(int_start) >> INT_BLOCK_SHIFT);
    stream_address_offset_ = int_start & INT_BLOCK_MASK;
    return static_cast<size_t>(id);
}

} // namespace lucene
