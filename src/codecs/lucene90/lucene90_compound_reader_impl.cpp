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

module lucene_core:lucene90_compound_reader.impl;

import :lucene90_compound_reader;

import :buffered_checksum_index_input;
import :check_sum_index_input;
import :codec_util;
import :compound_directory;
import :data_input;
import :directory;
import :error;
import :index_input;
import :index_output;
import :io_context;
import :lock;
import :segment_info;
import :types;

import std;

namespace lucene {

namespace {

/// Mirrors `kDataExtension` / `ENTRIES_EXTENSION`.
/// They are repeated here because the format imports this partition, and a
/// module cycle is not worth two string literals.
constexpr std::string_view kDataExtension = "cfs";
constexpr std::string_view kEntriesExtension = "cfe";
/// Mirrors `Lucene90CompoundFormat::DATA_CODEC`.
constexpr std::string_view kDataCodec = "Lucene90CompoundData";
/// Mirrors `Lucene90CompoundFormat::ENTRY_CODEC`.
constexpr std::string_view kEntryCodec = "Lucene90CompoundEntries";
/// Mirrors `Lucene90CompoundFormat::VERSION_START` / `VERSION_CURRENT`.
constexpr int32_t kVERSION_START = 0;
constexpr int32_t kVERSION_CURRENT = kVERSION_START;

/// rlucene's one-argument `IndexFileNames::strip_segment_name`: everything from
/// the first `_` on, without the `_` itself.  The two-argument overload in
/// `:index_file_names` does something else (it removes a known base).
std::string strip_segment_name(std::string_view name) {
    const auto pos = name.find('_');
    if (pos == std::string_view::npos) {
        return std::string(name);
    }
    return std::string(name.substr(pos + 1));
}

} // namespace

Lucene90CompoundReader::Lucene90CompoundReader(std::string segment_name,
                                               std::unordered_map<std::string, FileEntry> entries,
                                               std::unique_ptr<IndexInput> handle, std::string dir_fmt)
    : segment_name_(std::move(segment_name)), entries_(std::move(entries)), handle_(std::move(handle)),
      dir_fmt_(std::move(dir_fmt)) {}

Result<std::unique_ptr<Lucene90CompoundReader>> Lucene90CompoundReader::create(Directory &directory,
                                                                              const SegmentInfo &si) {
    const std::string data_file_name = segment_file_name(si.name, "", kDataExtension);
    const std::string entries_file_name = segment_file_name(si.name, "", kEntriesExtension);

    std::unordered_map<std::string, FileEntry> entries;
    auto version = read_entries(si.id, directory, entries_file_name, entries);
    if (!version) {
        return std::unexpected(std::move(version.error()));
    }

    auto handle = directory.open_input(data_file_name, io_context_default());
    if (!handle) {
        return std::unexpected(std::move(handle.error()));
    }

    size_t expected_length = CodecUtil::header_length(kDataCodec);
    for (const auto &[name, entry] : entries) {
        static_cast<void>(name);
        expected_length = std::max(expected_length, entry.offset + entry.length);
    }
    expected_length += CodecUtil::footer_length();

    if (auto checked = CodecUtil::check_index_header(*handle.value(), kDataCodec,
                                                     version.value(), version.value(), si.id, "");
        !checked) {
        return std::unexpected(std::move(checked.error()));
    }
    // The data file is too costly to verify checksum against all the bytes on
    // open, but at least verify the structure of the checksum footer: which
    // looks for FOOTER_MAGIC + algorithmID.  This is cheap and can detect some
    // forms of corruption such as file truncation.
    if (auto checksum = CodecUtil::retrieve_checksum(*handle.value()); !checksum) {
        return std::unexpected(std::move(checksum.error()));
    }

    // We also validate length, because e.g. if you strip 16 bytes off the .cfs
    // we otherwise would not detect it.
    auto length = handle.value()->length();
    if (!length) {
        return std::unexpected(std::move(length.error()));
    }
    if (length.value() != expected_length) {
        return err(LuceneError::corrupt_index("length should be " + std::to_string(expected_length) +
                                              " bytes, but is " + std::to_string(length.value()) +
                                              " instead (resource=" + std::string(handle.value()->to_string()) + ")"));
    }

    return std::unique_ptr<Lucene90CompoundReader>(new Lucene90CompoundReader(
            si.name, std::move(entries), std::move(handle.value()), directory.to_string()));
}

Result<int32_t> Lucene90CompoundReader::read_entries(std::span<const uint8_t> segment_id, Directory &directory,
                                                     std::string_view entries_file_name,
                                                     std::unordered_map<std::string, FileEntry> &mapping) {
    auto entries_stream = directory.open_checksum_input(entries_file_name);
    if (!entries_stream) {
        return std::unexpected(std::move(entries_stream.error()));
    }

    auto version = CodecUtil::check_index_header(*entries_stream.value(), kEntryCodec,
                                                 kVERSION_START,
                                                 kVERSION_CURRENT, segment_id, "");
    if (!version) {
        return std::unexpected(std::move(version.error()));
    }

    auto read = read_mapping(*entries_stream.value());
    if (!read) {
        return std::unexpected(std::move(read.error()));
    }
    mapping = std::move(read.value());

    if (auto footer = CodecUtil::check_footer(*entries_stream.value()); !footer) {
        return std::unexpected(std::move(footer.error()));
    }
    return version.value();
}

Result<std::unordered_map<std::string, FileEntry>>
Lucene90CompoundReader::read_mapping(IndexInput &entries_stream) {
    auto num_entries = entries_stream.read_vint();
    if (!num_entries) {
        return std::unexpected(std::move(num_entries.error()));
    }
    if (num_entries.value() < 0) {
        return err(LuceneError::corrupt_index("negative number of compound file entries"));
    }

    std::unordered_map<std::string, FileEntry> mapping;
    mapping.reserve(static_cast<size_t>(num_entries.value()));
    for (int32_t i = 0; i < num_entries.value(); ++i) {
        auto id = entries_stream.read_string();
        if (!id) {
            return std::unexpected(std::move(id.error()));
        }
        if (mapping.contains(id.value())) {
            return err(LuceneError::corrupt_index("Duplicate cfs entry id=" + id.value() + " in CFS (resource=" +
                                                  std::string(entries_stream.to_string()) + ")"));
        }
        auto offset = entries_stream.read_long();
        if (!offset) {
            return std::unexpected(std::move(offset.error()));
        }
        auto length = entries_stream.read_long();
        if (!length) {
            return std::unexpected(std::move(length.error()));
        }
        if (offset.value() < 0 || length.value() < 0) {
            return err(LuceneError::corrupt_index("negative compound file entry for id=" + id.value()));
        }
        mapping.emplace(id.value(), FileEntry{static_cast<size_t>(offset.value()), static_cast<size_t>(length.value())});
    }
    return mapping;
}

Result<std::vector<std::string>> Lucene90CompoundReader::list_all() const {
    std::vector<std::string> names;
    names.reserve(entries_.size());
    for (const auto &[id, entry] : entries_) {
        static_cast<void>(entry);
        // **Deviation:** rlucene concatenates `segment_name + id`, which loses
        // the separator (the id already starts with the segment's digits); this
        // port restores it so the names match the files in the parent
        // directory.
        names.push_back("_" + id);
    }
    std::ranges::sort(names);
    return names;
}

VoidResult Lucene90CompoundReader::delete_file(std::string_view /*name*/) {
    return err(LuceneError::unsupported_operation("delete_file"));
}

Result<size_t> Lucene90CompoundReader::file_length(std::string_view name) const {
    const std::string stripped_name = strip_segment_name(name);
    const auto entry = entries_.find(stripped_name);
    if (entry == entries_.end()) {
        return err(LuceneError::not_such_file(std::string(name) + " not found"));
    }
    return entry->second.length;
}

Result<std::unique_ptr<IndexOutput>> Lucene90CompoundReader::create_output(std::string_view /*name*/,
                                                                          const IOContext & /*context*/) {
    return err(LuceneError::unsupported_operation("create_output"));
}

Result<CreatedOutput> Lucene90CompoundReader::create_temp_output(std::string_view /*prefix*/,
                                                                 std::string_view /*suffix*/,
                                                                 const IOContext & /*context*/) {
    return err(LuceneError::unsupported_operation("create_temp_output"));
}

VoidResult Lucene90CompoundReader::sync(std::span<const std::string> /*names*/) {
    return err(LuceneError::unsupported_operation("sync"));
}

VoidResult Lucene90CompoundReader::rename(std::string_view /*source*/, std::string_view /*dest*/) {
    return err(LuceneError::unsupported_operation("rename"));
}

Result<std::unique_ptr<IndexInput>> Lucene90CompoundReader::open_input(std::string_view name,
                                                                      const IOContext &context) const {
    const std::string id = strip_segment_name(name);
    const auto entry = entries_.find(id);
    if (entry == entries_.end()) {
        const std::string data_file_name =
                segment_file_name(segment_name_, "", kDataExtension);
        return err(LuceneError::not_such_file("No sub-file with id " + id + " found in compound file \"" +
                                              data_file_name + "\" (fileName=" + std::string(name) + ")"));
    }

    static_cast<void>(context);
    std::lock_guard<std::mutex> guard(handle_mutex_);
    return handle_->slice(name, entry->second.offset, entry->second.length);
}

Result<std::unique_ptr<Lock>> Lucene90CompoundReader::obtain_lock(std::string_view /*name*/) {
    return err(LuceneError::unsupported_operation("obtain_lock"));
}

std::string Lucene90CompoundReader::to_string() const {
    return "CompoundFileDirectory(segment=\"" + segment_name_ + "\" in dir=" + dir_fmt_ + ")";
}

VoidResult Lucene90CompoundReader::check_integrity() const {
    std::lock_guard<std::mutex> guard(handle_mutex_);
    if (auto checksum = CodecUtil::checksum_entire_file(*handle_); !checksum) {
        return std::unexpected(std::move(checksum.error()));
    }
    return {};
}

} // namespace lucene
