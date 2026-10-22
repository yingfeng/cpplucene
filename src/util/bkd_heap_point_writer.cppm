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

export module lucene_core:bkd_heap_point_writer;

import :types;
import :error;

import std;

import :bkd_config;

namespace lucene {

/// A cursor into the packed block of a [`HeapPointWriter`].
///
/// Port of `HeapPointValue`.  The Rust version is thread through an enum
/// (`PointValueEnum::Heap`/`Offline`) because the offline writer keeps its
/// points on disk; this port only implements the heap flavour so far, so the
/// enum disappears and the type is used directly.
export class HeapPointValue {
public:
    explicit HeapPointValue(const BKDConfig &config, std::vector<uint8_t> value = {});

    void set_offset(size_t offset) noexcept { offset_ = offset; }
    [[nodiscard]] size_t offset() const noexcept { return offset_; }

    /// The `num_dims * bytes_per_dim` packed dimensions at [`offset`].
    [[nodiscard]] std::span<const uint8_t> packed_value() const;

    /// The big-endian document id stored right after the packed dimensions.
    [[nodiscard]] int32_t doc_id() const;

    /// The packed dimensions followed by the document id.
    [[nodiscard]] std::span<const uint8_t> packed_value_doc_id_bytes() const;

    [[nodiscard]] std::vector<uint8_t> &value() noexcept { return value_; }
    [[nodiscard]] const std::vector<uint8_t> &value() const noexcept { return value_; }

private:
    size_t offset_{0};
    std::vector<uint8_t> value_;
    size_t packed_value_length_{0};
    size_t packed_value_doc_id_length_{0};
};

/// Reads a contiguous slice of a [`HeapPointWriter`]'s points.
///
/// Port of `HeapPointReader`.  `next()` starts the cursor one position before
/// `start`, so the first call lands on `start`.
export class HeapPointReader {
public:
    HeapPointReader(HeapPointValue points, size_t start, size_t end, size_t bytes_per_doc);

    /// Advances the cursor; `false` once `end` is reached.
    [[nodiscard]] bool next();

    /// The packed dimensions of the current point.
    [[nodiscard]] std::span<const uint8_t> packed_value();

    /// The document id of the current point.
    [[nodiscard]] int32_t doc_id();

    [[nodiscard]] size_t position() const noexcept { return cur_read_; }
    [[nodiscard]] bool exhausted() const noexcept { return !started_ || cur_read_ >= end_; }
    void close() noexcept { started_ = false; }

private:
    void require_position() const;

    HeapPointValue points_;
    size_t cur_read_{0};
    size_t end_{0};
    size_t bytes_per_doc_{0};
    bool started_{false};
};

/// Writes new points into an in-heap byte block and sorts them in place.
///
/// Port of `HeapPointWriter` (`util/bkd/heap_point_write.rs`).
///
/// Layout per point: `num_dims * bytes_per_dim` packed dimensions, then the
/// indexed dimensions (`num_index_dims * bytes_per_dim`) are the leading part of
/// that, and the big-endian doc id terminates the record.  Comparisons use the
/// unsigned byte order of these fields.
///
/// **Deviation:** rlucene returns `Result` from the accessors because
/// `point_value` may hold an offline writer.  With a single flavour the accessors
/// assert instead, which keeps the BKD writer readable.
export class HeapPointWriter {
public:
    /// `size` is the maximum number of points the block can hold.
    HeapPointWriter(const BKDConfig &config, size_t size);

    /// Appends one point; `packed_value` must be `packed_bytes_length()` long.
    [[nodiscard]] VoidResult append_bytes(std::span<const uint8_t> packed_value, int32_t doc_id);

    /// Appends a record that already contains the doc id (`bytes_per_doc`).
    [[nodiscard]] VoidResult append_packed(std::span<const uint8_t> packed_value_with_doc_id);

    /// Freezes the writer and hands the block over to the value cursor.
    VoidResult close();
    [[nodiscard]] bool closed() const noexcept { return closed_; }

    [[nodiscard]] size_t count() const noexcept { return next_write_; }
    [[nodiscard]] size_t capacity() const noexcept { return size_; }
    [[nodiscard]] const BKDConfig &config() const noexcept { return config_; }

    [[nodiscard]] int32_t byte_at(size_t i, size_t k) const;

    [[nodiscard]] VoidResult copy_dim(size_t i, size_t dim, std::span<uint8_t> bytes, size_t offset) const;
    [[nodiscard]] VoidResult copy_data_dims_and_doc(size_t i, std::span<uint8_t> bytes, size_t offset) const;

    [[nodiscard]] int32_t compare_dim(size_t i, size_t j, size_t dim) const;
    [[nodiscard]] int32_t compare_dim_with_scratch(size_t j, std::span<const uint8_t> dim_value, size_t offset,
                                                   size_t dim) const;
    [[nodiscard]] int32_t compare_data_dims_and_doc(size_t i, size_t j) const;
    [[nodiscard]] int32_t compare_data_dims_and_doc_with(size_t j, std::span<const uint8_t> data_dims_and_docs,
                                                         size_t offset) const;

    /// Number of distinct points in `[from, to)` when only the bytes past
    /// `common_prefix_lengths[dim]` are compared.
    [[nodiscard]] size_t compute_cardinality(size_t from, size_t to,
                                             std::span<const size_t> common_prefix_lengths) const;

    /// Cursor pointing at point `index`; the writer must be closed.
    [[nodiscard]] HeapPointValue &packed_value(size_t index);

    /// Swaps the points at `i` and `j`.
    void swap(size_t i, size_t j);

    /// A reader over `[start, start + length)`; transfers the block to it.
    [[nodiscard]] Result<HeapPointReader> make_reader(size_t start, size_t length);

    /// Moves the (already closed) block out, mirroring `mem::take` in rlucene.
    [[nodiscard]] std::vector<uint8_t> take_block() noexcept { return std::move(block_); }

    [[nodiscard]] std::string to_string() const;

private:
    void require_closed() const;
    [[nodiscard]] std::span<const uint8_t> point_bytes() const;

    std::vector<uint8_t> block_;
    size_t size_{0};
    BKDConfig config_;
    std::vector<uint8_t> scratch_;
    size_t data_dims_and_doc_length_{0};
    size_t next_write_{0};
    bool closed_{false};
    std::optional<HeapPointValue> point_value_;
};

} // namespace lucene
