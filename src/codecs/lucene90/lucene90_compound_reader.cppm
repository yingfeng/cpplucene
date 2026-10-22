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

export module lucene_core:lucene90_compound_reader;

import :types;
import std;

import :compound_directory;
import :directory;
import :error;
import :index_input;
import :index_output;
import :io_context;
import :lock;
import :segment_info;

namespace lucene {

/// Offset/Length for a slice inside of a compound file.
export struct FileEntry {
    size_t offset{0};
    size_t length{0};
};

/// Provides access to a compound stream.
///
/// This class implements [`Directory`] but is limited to read-only operations;
/// any directory method that attempts to modify data fails with
/// `unsupported_operation`.
///
/// Port of `Lucene90CompoundReader`
/// (`core/codecs/lucene90/lucene90_compound_reader.rs`).
///
/// **Deviations** (C++ mechanics only): the reference implementation wraps the shared handle in a
/// `parking_lot::Mutex`; this port uses `std::mutex` with a `mutable` handle so
/// that the `const` [`Directory::open_input`] can still slice it.
export class Lucene90CompoundReader final : public CompoundDirectory {
public:
    /// Opens the compound file of `si` in `directory`.
    [[nodiscard]] static Result<std::unique_ptr<Lucene90CompoundReader>> create(Directory &directory,
                                                                               const SegmentInfo &si);

    // ---- Directory ----

    [[nodiscard]] Result<std::vector<std::string>> list_all() const override;
    [[nodiscard]] VoidResult delete_file(std::string_view name) override;
    [[nodiscard]] Result<size_t> file_length(std::string_view name) const override;
    [[nodiscard]] Result<std::unique_ptr<IndexOutput>> create_output(std::string_view name,
                                                                     const IOContext &context) override;
    [[nodiscard]] Result<CreatedOutput> create_temp_output(std::string_view prefix, std::string_view suffix,
                                                           const IOContext &context) override;
    [[nodiscard]] VoidResult sync(std::span<const std::string> names) override;
    [[nodiscard]] VoidResult sync_metadata() override { return {}; }
    [[nodiscard]] VoidResult rename(std::string_view source, std::string_view dest) override;
    [[nodiscard]] Result<std::unique_ptr<IndexInput>> open_input(std::string_view name,
                                                                 const IOContext &context) const override;
    [[nodiscard]] Result<std::unique_ptr<Lock>> obtain_lock(std::string_view name) override;
    [[nodiscard]] std::string to_string() const override;

    // ---- CompoundDirectory ----

    [[nodiscard]] VoidResult check_integrity() const override;

    // ---- introspection (used by tests and the format) ----

    /// The sub-files of the compound file, keyed by name without the segment
    /// prefix.
    [[nodiscard]] const std::unordered_map<std::string, FileEntry> &entries() const noexcept { return entries_; }
    [[nodiscard]] const std::string &segment_name() const noexcept { return segment_name_; }

private:
    Lucene90CompoundReader(std::string segment_name, std::unordered_map<std::string, FileEntry> entries,
                           std::unique_ptr<IndexInput> handle, std::string dir_fmt);

    /// Reads the `.cfe` entry table; returns the version it was written with.
    [[nodiscard]] static Result<int32_t> read_entries(std::span<const uint8_t> segment_id, Directory &directory,
                                                      std::string_view entries_file_name,
                                                      std::unordered_map<std::string, FileEntry> &mapping);

    /// Reads the `<name, offset, length>` triples of a `.cfe` file.
    [[nodiscard]] static Result<std::unordered_map<std::string, FileEntry>> read_mapping(IndexInput &entries_stream);

    std::string segment_name_;
    std::unordered_map<std::string, FileEntry> entries_;
    mutable std::mutex handle_mutex_;
    mutable std::unique_ptr<IndexInput> handle_;
    std::string dir_fmt_;
};

} // namespace lucene
