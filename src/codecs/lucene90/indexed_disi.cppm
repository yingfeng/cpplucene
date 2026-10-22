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

export module lucene_core:indexed_disi;

import :types;
import :error;

import std;

import :data_input;
import :doc_id_set_iterator;
import :index_input;
import :index_output;
import :random_access_input;

namespace lucene {

/// Document ids per encoded block.
export constexpr int32_t DISI_BLOCK_SIZE = 65536;
/// Rank entries are written every `2^9` doc ids by default (every 512 doc ids).
export constexpr int8_t DEFAULT_DENSE_RANK_POWER = 9;
/// Largest cardinality still stored as a list of 16-bit doc ids.
///
/// A block with at most this many values uses the SPARSE encoding; above it the
/// block is a bit set (DENSE), and a block holding every doc id is ALL.
export constexpr int32_t DISI_MAX_ARRAY_LENGTH = (1 << 12) - 1;

/// What [`IndexedDISI::write_bitset`] left behind: the two numbers the caller has
/// to store in its metadata so the blocks can be read back.
export struct DisiWriteResult {
    /// Jump-table entries that follow the blocks, `-1` when there is none.
    int32_t jump_table_entry_count{-1};
    /// Rank spacing the writer used for DENSE blocks.
    int8_t dense_rank_power{DEFAULT_DENSE_RANK_POWER};
};

/// Disk-based [`DocIdSetIterator`] that can also report how many documents it has
/// already returned, which lets sparse doc values store a value only for the
/// documents that have one.
///
/// Port of `IndexedDISI`
/// (`codecs/lucene90/indexed_disi.rs`).  Documents are grouped in blocks of
/// 65536; each block that holds at least one document picks one of three
/// encodings:
///
///  * `ALL` — the block holds all 65536 documents (4 bytes of header only);
///  * `DENSE` — at least 4096 documents, stored as a 65536-bit set plus a rank
///    table every `2^dense_rank_power` documents;
///  * `SPARSE` — otherwise, the low 16 bits of each doc id as a `short`.
///
/// The blocks are followed by a jump table of `(index, offset)` pairs for fast
/// seeks across blocks.
///
/// **Deviations:**
///  * the jump table is read into a `std::vector` when the iterator is created
///    instead of being kept as a `RandomAccessInput`.  It is at most a few
///    hundred KiB and the read is sequential, and it keeps the iterator free of
///    a second lazily-created slice;
///  * the reference implementation's `Owned`/`Shared` policies and the `DocIndexIterator` wrapper are
///    collapsed: this class is the owned iterator and exposes [`index`] itself.
export class IndexedDISI final : public DocIdSetIterator {
public:
    /// Opens the blocks stored at `[offset, offset + length)` of `input`.
    ///
    /// `length` covers both the blocks and the jump table.  `cost` is normally
    /// the number of logical doc ids of the field.
    [[nodiscard]] static Result<std::unique_ptr<IndexedDISI>> create(const IndexInput &input, size_t offset,
                                                                    size_t length, int32_t jump_table_entry_count,
                                                                    int8_t dense_rank_power, int64_t cost);

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override;

    [[nodiscard]] Result<int32_t> advance(int32_t target) override;

    [[nodiscard]] Result<int64_t> cost() const override { return cost_; }

    /// Advances to exactly `target`, reporting whether `target` is present.
    ///
    /// Afterwards [`doc_id`] returns `target` either way.
    [[nodiscard]] Result<bool> advance_exact(int32_t target);

    /// Ordinal of the current document among the documents this iterator returns.
    [[nodiscard]] int32_t index() const noexcept { return index_; }

    /// Encoding of the block the iterator currently sits on.
    [[nodiscard]] bool is_all_block() const noexcept { return method_ == Method::kAll; }
    [[nodiscard]] bool is_dense_block() const noexcept { return method_ == Method::kDense; }
    [[nodiscard]] bool is_sparse_block() const noexcept { return method_ == Method::kSparse; }

    /// Writes `it`'s doc ids as blocks and appends the jump table.
    ///
    /// Returns the two values the caller must store to read the blocks back.
    [[nodiscard]] static Result<DisiWriteResult> write_bitset(DocIdSetIterator &it, IndexOutput &out,
                                                              int8_t dense_rank_power = DEFAULT_DENSE_RANK_POWER);

    /// Bytes of the jump table for `entry_count` entries.
    [[nodiscard]] static constexpr size_t jump_table_bytes(int32_t entry_count) noexcept {
        return entry_count < 0 ? 0 : static_cast<size_t>(entry_count) * 8;
    }

private:
    /// How the current block is encoded.
    enum class Method : uint8_t {
        kSparse = 0,
        kDense = 1,
        kAll = 2,
    };

    IndexedDISI(std::unique_ptr<IndexInput> slice, std::vector<int32_t> jump_table, int32_t jump_table_entry_count,
                int8_t dense_rank_power, int64_t cost);

    /// Seeks to the block that starts at `target_block`.
    [[nodiscard]] VoidResult advance_block(int32_t target_block);

    /// Reads the header of the block the slice points at and prepares its state.
    [[nodiscard]] VoidResult read_block_header();

    [[nodiscard]] Result<bool> advance_within_block(int32_t target);
    [[nodiscard]] Result<bool> advance_exact_within_block(int32_t target);

    /// Jumps inside a DENSE block using the rank table.
    [[nodiscard]] VoidResult rank_skip(int32_t target_in_block);

    /// The blocks and their jump table, without the jump table.
    std::unique_ptr<IndexInput> slice_;
    /// `(index, offset)` pairs, empty when the writer omitted the table.
    std::vector<int32_t> jump_table_;
    int32_t jump_table_entry_count_{0};
    int8_t dense_rank_power_{-1};
    /// Longs covered by one rank entry.
    int32_t dense_rank_words_{0};
    /// Scratch buffer for the rank table of the current DENSE block.
    std::vector<uint8_t> dense_rank_table_;
    int64_t cost_{0};

    int32_t block_{-1};
    size_t block_end_{0};
    std::optional<size_t> dense_bitmap_offset_;
    int32_t next_block_index_{-1};
    Method method_{Method::kSparse};
    int32_t doc_{-1};
    int32_t index_{-1};

    // SPARSE state.
    bool exists_{false};
    int32_t next_exist_doc_in_block_{0};

    // DENSE state.
    int64_t word_{0};
    int32_t word_index_{-1};
    /// One bits seen so far, including those of `word_.
    int32_t number_of_ones_{0};
    /// Absolute rank origin of the current DENSE block.
    int32_t dense_origo_index_{0};

    // ALL state.
    int32_t gap_{0};
};

} // namespace lucene
