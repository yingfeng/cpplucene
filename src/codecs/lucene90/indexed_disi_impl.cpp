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

module lucene_core:indexed_disi.impl;

import :indexed_disi;

import :bit_util;
import :data_input;
import :data_output;
import :doc_id_set_iterator;
import :error;
import :index_input;
import :index_output;
import :random_access_input;
import :types;

import std;

namespace lucene {

namespace {

/// Words in a DENSE block's bit set (`65536 / 64`).
constexpr size_t DENSE_BLOCK_LONGS = static_cast<size_t>(DISI_BLOCK_SIZE) / 64;

/// A 65536-bit buffer, one `uint64_t` per 64 doc ids.
using BitBuffer = std::vector<uint64_t>;

void set_bit(BitBuffer &buffer, int32_t bit) {
    buffer[static_cast<size_t>(bit) >> 6] |= uint64_t{1} << (static_cast<uint64_t>(bit) & 63);
}

[[nodiscard]] bool get_bit(const BitBuffer &buffer, int32_t bit) {
    return ((buffer[static_cast<size_t>(bit) >> 6] >> (static_cast<uint64_t>(bit) & 63)) & 1) != 0;
}

[[nodiscard]] bool valid_dense_rank_power(int8_t dense_rank_power) {
    return (dense_rank_power >= 7 && dense_rank_power <= 15) || dense_rank_power == -1;
}

/// Writes `bits` as one block: header, then the encoding payload.
[[nodiscard]] VoidResult flush_block(int32_t block, const BitBuffer &bits, int32_t cardinality,
                                     int8_t dense_rank_power, IndexOutput &out) {
    if (block < 0 || block >= DISI_BLOCK_SIZE) {
        return err(LuceneError::illegal_argument("the block index is out of range: " + std::to_string(block)));
    }
    if (auto written = out.write_short(static_cast<int16_t>(block)); !written.has_value()) {
        return written;
    }
    if (cardinality <= 0 || cardinality > DISI_BLOCK_SIZE) {
        return err(LuceneError::illegal_argument("the block cardinality is out of range"));
    }
    if (auto written = out.write_short(static_cast<int16_t>(cardinality - 1)); !written.has_value()) {
        return written;
    }

    if (cardinality > DISI_MAX_ARRAY_LENGTH) {
        if (cardinality == DISI_BLOCK_SIZE) {
            // ALL: the bit set is implied by the header.
            return {};
        }
        if (dense_rank_power != -1) {
            // Ranks: the number of set bits before every `2^dense_rank_power` doc
            // ids, two bytes each, big endian.
            const size_t longs_per_rank = size_t{1} << (dense_rank_power - 6);
            const size_t rank_index_shift = static_cast<size_t>(dense_rank_power - 7);
            const size_t rank_len = DENSE_BLOCK_LONGS >> rank_index_shift;
            std::vector<uint8_t> rank(rank_len, 0);
            int32_t bit_count = 0;
            for (size_t word = 0; word < DENSE_BLOCK_LONGS; ++word) {
                if ((word & (longs_per_rank - 1)) == 0) {
                    const size_t rank_index = word >> rank_index_shift;
                    rank[rank_index] = static_cast<uint8_t>(bit_count >> 8);
                    rank[rank_index + 1] = static_cast<uint8_t>(bit_count & 0xFF);
                }
                bit_count += std::popcount(bits[word]);
            }
            if (auto written = out.write_bytes(std::span<const uint8_t>(rank)); !written.has_value()) {
                return written;
            }
        }
        for (const uint64_t word : bits) {
            if (auto written = out.write_long(static_cast<int64_t>(word)); !written.has_value()) {
                return written;
            }
        }
        return {};
    }

    // SPARSE: the low 16 bits of every doc id in the block.
    for (int32_t bit = 0; bit < DISI_BLOCK_SIZE; ++bit) {
        if (!get_bit(bits, bit)) {
            continue;
        }
        if (auto written = out.write_short(static_cast<int16_t>(bit)); !written.has_value()) {
            return written;
        }
    }
    return {};
}

/// Records `(index, offset)` for every empty block between `start_block` and
/// `end_block`, so a reader can jump over blocks that hold nothing.
void add_jumps(std::vector<int32_t> &jumps, size_t offset, int32_t index, size_t start_block, size_t end_block) {
    const size_t needed = (end_block + 1) * 2;
    if (jumps.size() < needed) {
        jumps.resize(needed, 0);
    }
    const auto stored_offset = static_cast<int32_t>(offset);
    for (size_t block = start_block; block < end_block; ++block) {
        jumps[block * 2] = index;
        jumps[block * 2 + 1] = stored_offset;
    }
}

/// Appends the jump table and reports how many entries it holds (`0` when it was
/// not worth writing).
[[nodiscard]] VoidResult flush_block_jumps(const std::vector<int32_t> &jumps, size_t block_count, IndexOutput &out,
                                           int32_t &entries_written) {
    // A table with a single real entry plus the NO_MORE_DOCS block is pure
    // overhead, so it is dropped.
    if (block_count == 2) {
        block_count = 0;
    }
    for (size_t i = 0; i < block_count; ++i) {
        if (auto written = out.write_int(jumps[i * 2]); !written.has_value()) {
            return written;
        }
        if (auto written = out.write_int(jumps[i * 2 + 1]); !written.has_value()) {
            return written;
        }
    }
    entries_written = static_cast<int32_t>(block_count);
    return {};
}

} // namespace

IndexedDISI::IndexedDISI(std::unique_ptr<IndexInput> slice, std::vector<int32_t> jump_table,
                         int32_t jump_table_entry_count, int8_t dense_rank_power, int64_t cost)
    : slice_(std::move(slice)),
      jump_table_(std::move(jump_table)),
      jump_table_entry_count_(jump_table_entry_count),
      dense_rank_power_(dense_rank_power),
      dense_rank_words_(dense_rank_power == -1 ? 0 : (1 << (dense_rank_power - 6))),
      cost_(cost) {
    if (dense_rank_power_ != -1) {
        const size_t rank_index_shift = static_cast<size_t>(dense_rank_power_ - 7);
        dense_rank_table_.assign(DENSE_BLOCK_LONGS >> rank_index_shift, 0);
    }
}

Result<std::unique_ptr<IndexedDISI>> IndexedDISI::create(const IndexInput &input, size_t offset, size_t length,
                                                         int32_t jump_table_entry_count, int8_t dense_rank_power,
                                                         int64_t cost) {
    if (!valid_dense_rank_power(dense_rank_power)) {
        return err(LuceneError::illegal_argument(
                "acceptable values for denseRankPower are 7-15 or -1, got " + std::to_string(dense_rank_power)));
    }
    const size_t jump_bytes = jump_table_bytes(jump_table_entry_count);
    if (length < jump_bytes) {
        return err(LuceneError::corrupt_index("the indexed doc id set is shorter than its jump table"));
    }

    auto slice = input.slice("docs", offset, length - jump_bytes);
    if (!slice.has_value()) {
        return std::unexpected(std::move(slice.error()));
    }

    std::vector<int32_t> table;
    if (jump_table_entry_count > 0) {
        auto jump_input = input.random_access_slice(offset + length - jump_bytes, jump_bytes);
        if (!jump_input.has_value()) {
            return std::unexpected(std::move(jump_input.error()));
        }
        table.resize(static_cast<size_t>(jump_table_entry_count) * 2);
        for (size_t i = 0; i < table.size(); ++i) {
            auto value = jump_input.value()->read_int(i * BitUtil::INT_BYTES);
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            table[i] = value.value();
        }
    }

    std::unique_ptr<IndexedDISI> disi(new IndexedDISI(std::move(slice.value()), std::move(table),
                                                     jump_table_entry_count, dense_rank_power, cost));
    if (auto length_here = disi->slice_->length(); length_here.has_value() && length_here.value() > 0) {
        if (auto prefetched = disi->slice_->prefetch(0, 1); !prefetched.has_value()) {
            return std::unexpected(std::move(prefetched.error()));
        }
    }
    return disi;
}

VoidResult IndexedDISI::read_block_header() {
    auto block_short = slice_->read_short();
    if (!block_short.has_value()) {
        return std::unexpected(std::move(block_short.error()));
    }
    block_ = static_cast<int32_t>(static_cast<uint16_t>(block_short.value())) << 16;

    auto count_short = slice_->read_short();
    if (!count_short.has_value()) {
        return std::unexpected(std::move(count_short.error()));
    }
    const int32_t num_values = 1 + static_cast<int32_t>(static_cast<uint16_t>(count_short.value()));

    index_ = next_block_index_;
    next_block_index_ = index_ + num_values;

    auto position = slice_->get_file_pointer();
    if (!position.has_value()) {
        return std::unexpected(std::move(position.error()));
    }

    if (num_values <= DISI_MAX_ARRAY_LENGTH) {
        method_ = Method::kSparse;
        block_end_ = position.value() + static_cast<size_t>(num_values) * BitUtil::SHORT_BYTES;
        next_exist_doc_in_block_ = -1;
        return {};
    }
    if (num_values == DISI_BLOCK_SIZE) {
        method_ = Method::kAll;
        block_end_ = position.value();
        gap_ = block_ - index_ - 1;
        return {};
    }

    method_ = Method::kDense;
    const size_t dense_bitmap_offset = position.value() + dense_rank_table_.size();
    dense_bitmap_offset_ = dense_bitmap_offset;
    block_end_ = dense_bitmap_offset + (size_t{1} << 13);
    if (dense_rank_power_ != -1) {
        // The whole rank table is read up front: it is small and a DENSE block is
        // usually walked in order.
        if (auto read = slice_->read_bytes(dense_rank_table_); !read.has_value()) {
            return read;
        }
    }
    word_index_ = -1;
    number_of_ones_ = index_ + 1;
    dense_origo_index_ = number_of_ones_;
    return {};
}

VoidResult IndexedDISI::advance_block(int32_t target_block) {
    const int32_t block_index = target_block >> 16;
    // Two or more blocks ahead: use the jump table.
    if (!jump_table_.empty() && block_index >= (block_ >> 16) + 2) {
        const int32_t in_range = block_index < jump_table_entry_count_ ? block_index : jump_table_entry_count_ - 1;
        const size_t entry = static_cast<size_t>(in_range) * 2;
        if (entry + 1 >= jump_table_.size()) {
            return err(LuceneError::corrupt_index("the jump table entry is out of range"));
        }
        // `-1` compensates for the `+1` of the first block's index.
        next_block_index_ = jump_table_[entry] - 1;
        const auto offset = static_cast<size_t>(jump_table_[entry + 1]);
        if (auto seeked = slice_->seek(offset); !seeked.has_value()) {
            return seeked;
        }
        return read_block_header();
    }

    // Fall back to walking the blocks.
    while (true) {
        if (auto seeked = slice_->seek(block_end_); !seeked.has_value()) {
            return seeked;
        }
        if (auto header = read_block_header(); !header.has_value()) {
            return header;
        }
        if (block_ >= target_block) {
            return {};
        }
    }
}

VoidResult IndexedDISI::rank_skip(int32_t target_in_block) {
    if (dense_rank_power_ < 0 || dense_rank_table_.empty() || !dense_bitmap_offset_.has_value()) {
        return err(LuceneError::illegal_state("the rank table is not available for this block"));
    }
    const int32_t rank_index = target_in_block >> dense_rank_power_;
    const size_t byte_index = static_cast<size_t>(rank_index) * 2;
    if (byte_index + 1 >= dense_rank_table_.size()) {
        return err(LuceneError::corrupt_index("the rank entry is out of range"));
    }
    const int32_t rank = (static_cast<int32_t>(dense_rank_table_[byte_index]) << 8) |
                         static_cast<int32_t>(dense_rank_table_[byte_index + 1]);

    const int32_t rank_aligned_word_index = (rank_index << dense_rank_power_) >> 6;
    const size_t offset = dense_bitmap_offset_.value() + static_cast<size_t>(rank_aligned_word_index) *
                                                                   BitUtil::LONG_BYTES;
    if (auto seeked = slice_->seek(offset); !seeked.has_value()) {
        return seeked;
    }
    auto rank_word = slice_->read_long();
    if (!rank_word.has_value()) {
        return std::unexpected(std::move(rank_word.error()));
    }

    word_index_ = rank_aligned_word_index;
    word_ = rank_word.value();
    number_of_ones_ = dense_origo_index_ + rank + std::popcount(static_cast<uint64_t>(word_));
    return {};
}

Result<bool> IndexedDISI::advance_within_block(int32_t target) {
    const int32_t target_in_block = target & 0xFFFF;

    if (method_ == Method::kSparse) {
        while (index_ < next_block_index_) {
            auto doc_short = slice_->read_short();
            if (!doc_short.has_value()) {
                return std::unexpected(std::move(doc_short.error()));
            }
            const int32_t doc = static_cast<int32_t>(static_cast<uint16_t>(doc_short.value()));
            ++index_;
            if (doc >= target_in_block) {
                doc_ = block_ | doc;
                exists_ = true;
                next_exist_doc_in_block_ = doc;
                return true;
            }
        }
        return false;
    }

    if (method_ == Method::kAll) {
        doc_ = target;
        index_ = target - gap_;
        return true;
    }

    // DENSE.
    const int32_t target_word_index = static_cast<int32_t>(static_cast<uint32_t>(target_in_block) >> 6);
    if (dense_rank_power_ != -1 && target_word_index - word_index_ >= dense_rank_words_) {
        if (auto skipped = rank_skip(target_in_block); !skipped.has_value()) {
            return std::unexpected(std::move(skipped.error()));
        }
    }

    for (int32_t i = word_index_ + 1; i <= target_word_index; ++i) {
        auto read = slice_->read_long();
        if (!read.has_value()) {
            return std::unexpected(std::move(read.error()));
        }
        word_ = read.value();
        number_of_ones_ += std::popcount(static_cast<uint64_t>(word_));
    }
    word_index_ = target_word_index;

    const uint64_t left_bits = static_cast<uint64_t>(word_) >> (static_cast<uint64_t>(target_in_block) & 63);
    if (left_bits != 0) {
        doc_ = target + std::countr_zero(left_bits);
        index_ = number_of_ones_ - std::popcount(left_bits);
        return true;
    }

    while (true) {
        ++word_index_;
        if (word_index_ >= static_cast<int32_t>(DENSE_BLOCK_LONGS)) {
            break;
        }
        auto read = slice_->read_long();
        if (!read.has_value()) {
            return std::unexpected(std::move(read.error()));
        }
        word_ = read.value();
        if (word_ != 0) {
            index_ = number_of_ones_;
            number_of_ones_ += std::popcount(static_cast<uint64_t>(word_));
            doc_ = block_ | ((word_index_ << 6) | std::countr_zero(static_cast<uint64_t>(word_)));
            return true;
        }
    }
    return false;
}

Result<bool> IndexedDISI::advance_exact_within_block(int32_t target) {
    const int32_t target_in_block = target & 0xFFFF;

    if (method_ == Method::kSparse) {
        if (next_exist_doc_in_block_ > target_in_block) {
            exists_ = false;
            return false;
        }
        if (doc_ == target) {
            return exists_;
        }
        while (index_ < next_block_index_) {
            auto doc_short = slice_->read_short();
            if (!doc_short.has_value()) {
                return std::unexpected(std::move(doc_short.error()));
            }
            const int32_t doc = static_cast<int32_t>(static_cast<uint16_t>(doc_short.value()));
            ++index_;
            if (doc >= target_in_block) {
                next_exist_doc_in_block_ = doc;
                if (doc != target_in_block) {
                    --index_;
                    auto position = slice_->get_file_pointer();
                    if (!position.has_value()) {
                        return std::unexpected(std::move(position.error()));
                    }
                    if (auto seeked = slice_->seek(position.value() - BitUtil::SHORT_BYTES); !seeked.has_value()) {
                        return std::unexpected(std::move(seeked.error()));
                    }
                    break;
                }
                exists_ = true;
                return true;
            }
        }
        exists_ = false;
        return false;
    }

    if (method_ == Method::kAll) {
        index_ = target - gap_;
        return true;
    }

    // DENSE.
    const int32_t target_word_index = static_cast<int32_t>(static_cast<uint32_t>(target_in_block) >> 6);
    if (dense_rank_power_ != -1 && target_word_index - word_index_ >= dense_rank_words_) {
        if (auto skipped = rank_skip(target_in_block); !skipped.has_value()) {
            return std::unexpected(std::move(skipped.error()));
        }
    }
    for (int32_t i = word_index_ + 1; i <= target_word_index; ++i) {
        auto read = slice_->read_long();
        if (!read.has_value()) {
            return std::unexpected(std::move(read.error()));
        }
        word_ = read.value();
        number_of_ones_ += std::popcount(static_cast<uint64_t>(word_));
    }
    word_index_ = target_word_index;

    const int64_t left_bits = static_cast<int64_t>(static_cast<uint64_t>(word_) >>
                                                  (static_cast<uint64_t>(target_in_block) & 63));
    index_ = number_of_ones_ - std::popcount(static_cast<uint64_t>(left_bits));
    return (left_bits & 1) != 0;
}

Result<int32_t> IndexedDISI::next_doc() {
    if (doc_ == NO_MORE_DOCS) {
        return NO_MORE_DOCS;
    }
    return advance(doc_ + 1);
}

Result<int32_t> IndexedDISI::advance(int32_t target) {
    const int32_t target_block = static_cast<int32_t>(static_cast<uint32_t>(target) & 0xFFFF0000u);

    if (block_ < target_block) {
        if (auto advanced = advance_block(target_block); !advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
    }

    if (block_ == target_block) {
        auto advanced = advance_within_block(target);
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        if (advanced.value()) {
            return doc_;
        }
        if (auto header = read_block_header(); !header.has_value()) {
            return std::unexpected(std::move(header.error()));
        }
    }

    auto found = advance_within_block(block_);
    if (!found.has_value()) {
        return std::unexpected(std::move(found.error()));
    }
    // A block always holds at least one doc id, so this cannot come up empty.
    return doc_;
}

Result<bool> IndexedDISI::advance_exact(int32_t target) {
    const int32_t target_block = static_cast<int32_t>(static_cast<uint32_t>(target) & 0xFFFF0000u);

    if (block_ < target_block) {
        if (auto advanced = advance_block(target_block); !advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
    }

    bool found = false;
    if (block_ == target_block) {
        auto exact = advance_exact_within_block(target);
        if (!exact.has_value()) {
            return std::unexpected(std::move(exact.error()));
        }
        found = exact.value();
    }
    doc_ = target;
    return found;
}

Result<DisiWriteResult> IndexedDISI::write_bitset(DocIdSetIterator &it, IndexOutput &out, int8_t dense_rank_power) {
    if (!valid_dense_rank_power(dense_rank_power)) {
        return err(LuceneError::illegal_argument(
                "acceptable values for denseRankPower are 7-15 or -1, got " + std::to_string(dense_rank_power)));
    }

    auto origo = out.get_file_pointer();
    if (!origo.has_value()) {
        return std::unexpected(std::move(origo.error()));
    }

    int32_t total_cardinality = 0;
    int32_t block_cardinality = 0;
    BitBuffer buffer(DENSE_BLOCK_LONGS, 0);
    std::vector<int32_t> jumps;
    int32_t prev_block = -1;
    size_t jump_block_index = 0;

    auto doc = it.next_doc();
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    while (doc.value() != NO_MORE_DOCS) {
        const int32_t block = doc.value() >> 16;

        if (prev_block != -1 && block != prev_block) {
            auto position = out.get_file_pointer();
            if (!position.has_value()) {
                return std::unexpected(std::move(position.error()));
            }
            add_jumps(jumps, position.value() - origo.value(), total_cardinality, jump_block_index,
                      static_cast<size_t>(prev_block + 1));
            jump_block_index = static_cast<size_t>(prev_block + 1);
            if (auto flushed =
                        flush_block(prev_block, buffer, block_cardinality, dense_rank_power, out);
                !flushed.has_value()) {
                return std::unexpected(std::move(flushed.error()));
            }
            std::ranges::fill(buffer, 0);
            total_cardinality += block_cardinality;
            block_cardinality = 0;
        }

        set_bit(buffer, doc.value() & 0xFFFF);
        ++block_cardinality;
        prev_block = block;

        doc = it.next_doc();
        if (!doc.has_value()) {
            return std::unexpected(std::move(doc.error()));
        }
    }

    if (block_cardinality > 0) {
        auto position = out.get_file_pointer();
        if (!position.has_value()) {
            return std::unexpected(std::move(position.error()));
        }
        add_jumps(jumps, position.value() - origo.value(), total_cardinality, jump_block_index,
                  static_cast<size_t>(prev_block + 1));
        total_cardinality += block_cardinality;
        if (auto flushed = flush_block(prev_block, buffer, block_cardinality, dense_rank_power, out);
            !flushed.has_value()) {
            return std::unexpected(std::move(flushed.error()));
        }
        std::ranges::fill(buffer, 0);
        ++prev_block;
    }

    const size_t last_block = prev_block == -1 ? 0 : static_cast<size_t>(prev_block);
    // Every set ends with a block holding `NO_MORE_DOCS`, so that walking past the
    // last real doc id terminates instead of running off the slice.
    auto position = out.get_file_pointer();
    if (!position.has_value()) {
        return std::unexpected(std::move(position.error()));
    }
    add_jumps(jumps, position.value() - origo.value(), total_cardinality, last_block, last_block + 1);

    set_bit(buffer, NO_MORE_DOCS & 0xFFFF);
    if (auto flushed = flush_block(NO_MORE_DOCS >> 16, buffer, 1, dense_rank_power, out); !flushed.has_value()) {
        return std::unexpected(std::move(flushed.error()));
    }

    int32_t entry_count = 0;
    if (auto written = flush_block_jumps(jumps, last_block + 1, out, entry_count); !written.has_value()) {
        return std::unexpected(std::move(written.error()));
    }
    return DisiWriteResult{.jump_table_entry_count = entry_count, .dense_rank_power = dense_rank_power};
}

} // namespace lucene
