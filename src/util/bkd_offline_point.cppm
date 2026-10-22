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

export module lucene_core:bkd_offline_point;

import :types;
import :error;

import std;

import :bkd_config;
import :bkd_heap_point_writer;
import :directory;
import :index_input;
import :index_output;

namespace lucene {

/// The cursor type used by [`OfflinePointReader`].
///
/// The offline cursor is byte-for-byte the same as the on-heap one (an offset
/// plus a block of fixed-width records), so the reference implementation's `OfflinePointValue` is
/// an alias here rather than a second copy of the same code.
export using OfflinePointValue = HeapPointValue;

/// Reads fixed-width point records back from the temporary file written by an
/// [`OfflinePointWriter`].
///
/// Port of `OfflinePointReader` (`util/bkd/offline_point_reader.rs`), reading
/// through a reusable buffer of `n * bytes_per_doc` bytes.
///
/// **Deviation:** the reference implementation opens a checksum input when the slice covers the whole
/// file and validates the footer on close.  The footer is still written (so the
/// file looks like a codec file), but reading skips the checksum: the temporary
/// file is deleted right after the BKD build, and a partial slice could not be
/// checksummed anyway.
export class OfflinePointReader {
public:
    /// Opens `temp_file_name` and positions it at point `start`.
    ///
    /// Fails when the slice plus the codec footer does not fit into the file, or
    /// when `reusable_buffer` is too small to hold a single point.
    [[nodiscard]] static Result<OfflinePointReader> open(const BKDConfig &config, const Directory &dir,
                                                         std::string_view temp_file_name, size_t start, size_t length,
                                                         std::vector<uint8_t> reusable_buffer);

    /// Advances to the next point; `false` once the slice is exhausted.
    [[nodiscard]] Result<bool> next();

    /// The current point; call [`next`] first.
    [[nodiscard]] HeapPointValue &point_value();

    /// Number of points left in the slice.
    [[nodiscard]] size_t remaining() const noexcept { return count_left_ + points_in_buffer_; }

    [[nodiscard]] VoidResult close();

    [[nodiscard]] std::string to_string() const;

private:
    OfflinePointReader(const BKDConfig &config, std::unique_ptr<IndexInput> input, size_t length,
                       std::vector<uint8_t> reusable_buffer);

    size_t count_left_{0};
    std::unique_ptr<IndexInput> input_;
    size_t offset_{0};
    BKDConfig config_;
    size_t points_in_buffer_{0};
    size_t max_points_on_heap_{0};
    OfflinePointValue point_value_;
    bool closed_{false};
};

/// Writes fixed-width point records to a temporary file.
///
/// Port of `OfflinePointWriter` (`util/bkd/offline_point_write.rs`).  The file
/// is `count * bytes_per_doc` bytes followed by a codec footer, which is what
/// lets the BKD writer spill points that no longer fit in memory.
export class OfflinePointWriter {
public:
    /// Creates a uniquely named temporary file owned by `dir`.
    ///
    /// `expected_count` is only used for assertions (0 means "unknown").
    [[nodiscard]] static Result<OfflinePointWriter> create(const BKDConfig &config, Directory &dir,
                                                           std::string_view temp_file_name_prefix,
                                                           std::string_view desc, size_t expected_count);

    ~OfflinePointWriter();
    OfflinePointWriter(const OfflinePointWriter &) = delete;
    OfflinePointWriter &operator=(const OfflinePointWriter &) = delete;
    OfflinePointWriter(OfflinePointWriter &&) noexcept;
    OfflinePointWriter &operator=(OfflinePointWriter &&) noexcept;

    /// Appends `packed_value` (dimensions only) and its big-endian doc id.
    [[nodiscard]] VoidResult append_bytes(std::span<const uint8_t> packed_value, int32_t doc_id);

    /// Appends a record that already contains the doc id.
    [[nodiscard]] VoidResult append_packed(std::span<const uint8_t> record);

    /// Writes the codec footer and closes the file.
    [[nodiscard]] VoidResult close();

    /// Opens a reader over `[start, start + length)`.
    [[nodiscard]] Result<OfflinePointReader> make_reader(const Directory &dir, size_t start, size_t length,
                                                         std::vector<uint8_t> reusable_buffer) const;

    /// Deletes the temporary file.
    [[nodiscard]] VoidResult destroy(Directory &dir);

    [[nodiscard]] size_t count() const noexcept { return count_; }
    [[nodiscard]] const std::string &name() const noexcept { return name_; }
    [[nodiscard]] bool closed() const noexcept { return closed_; }
    [[nodiscard]] std::string to_string() const;

private:
    OfflinePointWriter(std::unique_ptr<IndexOutput> out, std::string name, const BKDConfig &config,
                       size_t expected_count);

    std::unique_ptr<IndexOutput> out_;
    std::string name_;
    BKDConfig config_;
    size_t count_{0};
    size_t expected_count_{0};
    bool closed_{false};
};

} // namespace lucene
