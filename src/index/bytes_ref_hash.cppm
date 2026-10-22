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

export module lucene_core:bytes_ref_hash;

import :types;
import std;

import :byte_block_pool;
import :bytes_ref;
import :bytes_ref_block_pool;
import :error;

namespace lucene {

/// Initial capacity of a [`BytesRefHash`].
export inline constexpr size_t DEFAULT_CAPACITY = 16;

/// Grows the `bytes_start` table of a [`BytesRefHash`].
///
/// Port of the `BytesStartArray` trait (`util/bytes_ref_hash.rs`).  The table
/// maps a term id to the offset of its bytes in the [`ByteBlockPool`].
export class BytesStartArray {
public:
    virtual ~BytesStartArray() = default;

    /// Allocates the initial table.
    [[nodiscard]] virtual VoidResult init() = 0;
    /// Grows the table so that one more term id fits.
    [[nodiscard]] virtual VoidResult grow() = 0;
    /// Releases the table.
    virtual void clear() = 0;
    [[nodiscard]] virtual Result<int32_t> get_value(size_t index) const = 0;
    [[nodiscard]] virtual VoidResult set_value(size_t index, int32_t value) = 0;
    [[nodiscard]] virtual Result<size_t> len() const = 0;
    [[nodiscard]] virtual bool need_init() const = 0;
};

/// The [`BytesStartArray`] that simply allocates.
export class DirectBytesStartArray final : public BytesStartArray {
public:
    explicit DirectBytesStartArray(size_t init_size) : init_size_(init_size) {}

    [[nodiscard]] VoidResult init() override;
    [[nodiscard]] VoidResult grow() override;
    void clear() override;
    [[nodiscard]] Result<int32_t> get_value(size_t index) const override;
    [[nodiscard]] VoidResult set_value(size_t index, int32_t value) override;
    [[nodiscard]] Result<size_t> len() const override { return bytes_start_.size(); }
    [[nodiscard]] bool need_init() const override { return !init_; }
    /// Bytes retained by the table; used for the RAM accounting of the writer.
    [[nodiscard]] size_t ram_bytes() const noexcept { return bytes_start_.size() * sizeof(int32_t); }

private:
    size_t init_size_{0};
    std::vector<int32_t> bytes_start_;
    bool init_{false};
};

/// A hash map from `BytesRef` to a dense `int32_t` id.
///
/// Port of `util/bytes_ref_hash.rs`.  The term bytes are stored once, in a
/// caller-owned [`ByteBlockPool`]; this type only keeps the id array (an open
/// addressing table with linear probing) and the `bytes_start` offsets.
///
/// [`add`] returns the id for a new term and `-(id + 1)` for a term that is
/// already present, so a single call both inserts and looks up.
///
/// **Deviations:**
///  * the RAM accounting is an on-demand estimate instead of a shared atomic
///    counter (the port has no `Accountable`/`Counter` infrastructure);
///  * [`sort`] uses the plain [`StringSorterBase`] path: the reference implementation's
///    `StringSorterImpl` additionally keeps a per-bucket cache in the tail of
///    the id array to make reordering cheaper.  That is a performance
///    optimisation only; the resulting order is the same.
export class BytesRefHash {
public:
    /// A hash with the default capacity.
    BytesRefHash();
    BytesRefHash(size_t capacity, std::unique_ptr<BytesStartArray> bytes_start_array);

    BytesRefHash(const BytesRefHash &) = delete;
    BytesRefHash &operator=(const BytesRefHash &) = delete;
    BytesRefHash(BytesRefHash &&) noexcept = default;
    BytesRefHash &operator=(BytesRefHash &&) noexcept = default;
    ~BytesRefHash() = default;

    /// Number of terms in the hash.
    [[nodiscard]] size_t size() const noexcept { return count_; }

    /// Where the term with id `bytes_id` lives in `pool`.
    [[nodiscard]] Result<BytesRefBlockPoolPosition> get(int32_t bytes_id, const ByteBlockPool &pool) const;

    /// Packs the ids to the front of the array; the ids are then valid in
    /// `[0, size())`.  Destructive: call [`clear`] before reusing the hash.
    [[nodiscard]] Result<const std::vector<int32_t> *> compact();

    /// Sorts the ids by term bytes; [`ids`] then holds the term ids in
    /// ascending term order.
    [[nodiscard]] VoidResult sort(const ByteBlockPool &byte_block_pool);

    /// Empties the hash, optionally resetting the byte pool too.
    void clear_with_reset_pool(bool reset_pool, ByteBlockPool &byte_block_pool);
    void clear(ByteBlockPool &byte_block_pool);
    /// Empties the hash and releases the id array.
    void close(ByteBlockPool &byte_block_pool);

    /// Adds `bytes`, returning its id, or `-(id + 1)` when already present.
    [[nodiscard]] Result<int32_t> add(const BytesRef &bytes, ByteBlockPool &byte_block_pool);

    /// The id of `bytes`, or `-1`.
    [[nodiscard]] Result<int32_t> find(const BytesRef &bytes, const ByteBlockPool &byte_block_pool) const;

    /// Like [`add`], but keyed by an arbitrary pool offset instead of a term.
    [[nodiscard]] Result<int32_t> add_by_pool_offset(int32_t offset, ByteBlockPool &byte_block_pool);

    /// Reallocates the id array after [`close`].
    [[nodiscard]] VoidResult reinit();

    /// The `bytes_start` offset of the term with id `bytes_id`.
    [[nodiscard]] Result<int32_t> byte_start(int32_t bytes_id) const;

    /// The id array; meaningful up to [`size`], or fully after [`compact`].
    [[nodiscard]] const std::vector<int32_t> &ids() const noexcept { return ids_; }

    /// The `bytes_start` table.
    ///
    /// The reference implementation exposes the field as `pub(crate)` because `TermsHashPerField`
    /// reads the postings array that lives inside its
    /// `PostingsBytesStartArray`.  The same access exists here (an accessor,
    /// so the hash keeps ownership).
    [[nodiscard]] BytesStartArray *bytes_start_array() noexcept { return bytes_start_array_.get(); }
    [[nodiscard]] const BytesStartArray *bytes_start_array() const noexcept { return bytes_start_array_.get(); }

    /// Estimated retained bytes, for the writer's RAM buffer accounting.
    [[nodiscard]] int64_t ram_bytes_used() const;

private:
    [[nodiscard]] Result<size_t> find_hash(const BytesRef &bytes, const ByteBlockPool &pool) const;
    /// Halves the table while it is at most 25% full; `true` when it resized.
    [[nodiscard]] bool shrink(size_t target_size);
    [[nodiscard]] VoidResult rehash(size_t new_size, bool hash_on_data, ByteBlockPool &byte_block_pool);

    BytesRefBlockPool pool_;
    size_t hash_size_{0};
    size_t hash_half_size_{0};
    size_t hash_mask_{0};
    size_t count_{0};
    std::optional<size_t> last_count_;
    std::vector<int32_t> ids_;
    std::unique_ptr<BytesStartArray> bytes_start_array_;
};

} // namespace lucene
