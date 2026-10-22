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

export module lucene_core:lucene90_compound_format;

import :types;
import std;

import :compound_directory;
import :compound_format;
import :directory;
import :error;
import :index_output;
import :io_context;
import :segment_info;

namespace lucene {

/// Lucene 9.0 compound file format.
///
/// # Files:
///
///  * `.cfs`: an optional "virtual" file consisting of all the other index files
///    for systems that frequently run out of file handles;
///  * `.cfe`: the "virtual" compound file's entry table, holding all entries in
///    the corresponding `.cfs` file.
///
/// # Description:
///
///  * Compound (`.cfs`) --> Header, FileData^FileCount, Footer
///  * Compound Entry Table (`.cfe`) --> Header, FileCount,
///    `<FileName, DataOffset, DataLength>`^FileCount
///  * FileData --> raw file data
///
/// # Notes:
///
///  * `FileCount` indicates how many files are contained in this compound file;
///  * each directory entry holds the start of the file's data section (aligned to
///    8 bytes, to avoid unaligned `mmap` accesses), the file's length and the
///    file's name minus its segment prefix.
///
/// Port of `Lucene90CompoundFormat`
/// (`core/codecs/lucene90/lucene90_compound_format.rs`).
export class Lucene90CompoundFormat final : public CompoundFormat {
public:
    /// Extension of the compound file.
    static constexpr std::string_view DATA_EXTENSION = "cfs";
    /// Extension of the compound file entries.
    static constexpr std::string_view ENTRIES_EXTENSION = "cfe";
    static constexpr std::string_view DATA_CODEC = "Lucene90CompoundData";
    static constexpr std::string_view ENTRY_CODEC = "Lucene90CompoundEntries";
    static constexpr int32_t VERSION_START = 0;
    static constexpr int32_t VERSION_CURRENT = VERSION_START;

    [[nodiscard]] Result<std::unique_ptr<CompoundDirectory>> get_compound_reader(Directory &dir,
                                                                                const SegmentInfo &si) const override;
    [[nodiscard]] VoidResult write(Directory &dir, const SegmentInfo &si, const IOContext &context) const override;

    /// Appends the files of `si` to `data`, writing their entry table to
    /// `entries`.
    ///
    /// Files are copied in increasing size order, which keeps the smaller files
    /// (the ones a reader opens first) at the front of the compound file.
    ///
    /// **Deviation:** the reference implementation drains a min-heap for this; the C++ port sorts a
    /// vector, which yields the same order without a `PriorityQueue` clone.
    [[nodiscard]] VoidResult write_compound_file(IndexOutput &entries, IndexOutput &data, Directory &directory,
                                                 const SegmentInfo &si) const;
};

} // namespace lucene
