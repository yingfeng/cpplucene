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

module lucene_core:lucene90_compound_format.impl;

import :lucene90_compound_format;

import :bit_util;
import :compound_directory;
import :compound_format;
import :codec_util;
import :data_input;
import :data_output;
import :directory;
import :error;
import :index_input;
import :index_output;
import :io_context;
import :lucene90_compound_reader;
import :segment_info;
import :types;

import std;

namespace lucene {

namespace {

/// One file to pack, with the size that decides the packing order.
struct SizedFile {
    std::string name;
    size_t length{0};
};

/// The reference implementation's one-argument `IndexFileNames::strip_segment_name`.
std::string strip_segment_name(std::string_view name) {
    const auto pos = name.find('_');
    if (pos == std::string_view::npos) {
        return std::string(name);
    }
    return std::string(name.substr(pos + 1));
}

} // namespace

Result<std::unique_ptr<CompoundDirectory>> Lucene90CompoundFormat::get_compound_reader(Directory &dir,
                                                                                      const SegmentInfo &si) const {
    auto reader = Lucene90CompoundReader::create(dir, si);
    if (!reader) {
        return std::unexpected(std::move(reader.error()));
    }
    return std::unique_ptr<CompoundDirectory>(std::move(reader.value()));
}

VoidResult Lucene90CompoundFormat::write(Directory &dir, const SegmentInfo &si, const IOContext &context) const {
    const std::string data_file = segment_file_name(si.name, "", DATA_EXTENSION);
    const std::string entries_file = segment_file_name(si.name, "", ENTRIES_EXTENSION);

    auto data = dir.create_output(data_file, context);
    if (!data) {
        return std::unexpected(std::move(data.error()));
    }
    auto entries = dir.create_output(entries_file, context);
    if (!entries) {
        if (auto closed = data.value()->close(); !closed) {
            return std::unexpected(std::move(closed.error()));
        }
        return std::unexpected(std::move(entries.error()));
    }

    auto write_all = [&]() -> VoidResult {
        if (auto header = CodecUtil::write_index_header(*data.value(), DATA_CODEC, VERSION_CURRENT, si.id, ""); !header) {
            return header;
        }
        if (auto header =
                    CodecUtil::write_index_header(*entries.value(), ENTRY_CODEC, VERSION_CURRENT, si.id, "");
            !header) {
            return header;
        }
        if (auto packed = write_compound_file(*entries.value(), *data.value(), dir, si); !packed) {
            return packed;
        }
        if (auto footer = CodecUtil::write_footer(*data.value()); !footer) {
            return footer;
        }
        return CodecUtil::write_footer(*entries.value());
    };

    auto written = write_all();
    // Close both outputs before reporting, so a failed write does not leak them.
    auto entries_closed = entries.value()->close();
    auto data_closed = data.value()->close();
    if (!written) {
        return std::unexpected(std::move(written.error()));
    }
    if (!entries_closed) {
        return std::unexpected(std::move(entries_closed.error()));
    }
    if (!data_closed) {
        return std::unexpected(std::move(data_closed.error()));
    }
    return {};
}

VoidResult Lucene90CompoundFormat::write_compound_file(IndexOutput &entries, IndexOutput &data,
                                                       Directory &directory, const SegmentInfo &si) const {
    std::vector<SizedFile> files;
    files.reserve(si.files.size());
    for (const auto &filename : si.files) {
        auto file_length = directory.file_length(filename);
        if (!file_length) {
            return std::unexpected(std::move(file_length.error()));
        }
        files.push_back(SizedFile{filename, file_length.value()});
    }

    // The size ordering is what the reference implementation's priority queue produced; ties are
    // broken by name so the output stays deterministic.
    std::ranges::sort(files, [](const SizedFile &left, const SizedFile &right) {
        if (left.length != right.length) {
            return left.length < right.length;
        }
        return left.name < right.name;
    });

    if (files.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        return err(LuceneError::illegal_argument("too many files to pack into a compound file"));
    }
    if (auto count = entries.write_vint(static_cast<int32_t>(files.size())); !count) {
        return count;
    }

    for (const auto &sized_file : files) {
        auto input = directory.open_checksum_input(sized_file.name);
        if (!input) {
            return std::unexpected(std::move(input.error()));
        }
        const auto &file = sized_file.name;

        auto aligned = data.align_file_pointer(BitUtil::LONG_BYTES);
        if (!aligned) {
            return std::unexpected(std::move(aligned.error()));
        }
        const size_t start_offset = aligned.value();

        auto copied = [&]() -> VoidResult {
            // Just copy the index header, verifying that its id matches what we
            // expect.
            if (auto header = CodecUtil::verify_and_copy_index_header(*input.value(), data, si.id); !header) {
                return header;
            }
            // Copy all bytes except the footer.
            auto length = input.value()->length();
            if (!length) {
                return std::unexpected(std::move(length.error()));
            }
            auto pointer = input.value()->get_file_pointer();
            if (!pointer) {
                return std::unexpected(std::move(pointer.error()));
            }
            const size_t num_bytes_to_copy = length.value() - CodecUtil::footer_length() - pointer.value();
            if (auto bytes = data.copy_bytes(*input.value(), num_bytes_to_copy); !bytes) {
                return bytes;
            }
            // Verify the footer (checksum) of the incoming file we are copying.
            auto checksum = CodecUtil::check_footer(*input.value());
            if (!checksum) {
                return std::unexpected(std::move(checksum.error()));
            }
            // This is poached from `write_footer`, but we need to use our own
            // checksum, not `data.get_checksum()`.
            if (auto written = CodecUtil::write_be_int(data, CodecUtil::FOOTER_MAGIC); !written) {
                return written;
            }
            if (auto written = CodecUtil::write_be_int(data, 0); !written) {
                return written;
            }
            return CodecUtil::write_be_long(data, checksum.value());
        };
        auto result = copied();
        auto closed = input.value()->close();
        if (!result) {
            return std::unexpected(std::move(result.error()));
        }
        if (!closed) {
            return std::unexpected(std::move(closed.error()));
        }

        auto end_offset = data.get_file_pointer();
        if (!end_offset) {
            return std::unexpected(std::move(end_offset.error()));
        }
        const size_t length = end_offset.value() - start_offset;

        if (auto id = entries.write_string(strip_segment_name(file)); !id) {
            return id;
        }
        if (auto offset = entries.write_long(static_cast<int64_t>(start_offset)); !offset) {
            return offset;
        }
        if (auto entry_length = entries.write_long(static_cast<int64_t>(length)); !entry_length) {
            return entry_length;
        }
    }
    return {};
}

} // namespace lucene
