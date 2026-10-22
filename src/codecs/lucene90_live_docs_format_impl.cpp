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

module lucene_core:lucene90_live_docs_format.impl;

import :lucene90_live_docs_format;

import :bits;
import :codec_util;
import :data_input;
import :data_output;
import :directory;
import :error;
import :fixed_bit_set;
import :index_file_names;
import :index_input;
import :index_output;
import :io_context;
import :live_docs_format;
import :segment_commit_info;
import :types;

import std;

namespace lucene {

std::string Lucene90LiveDocsFormat::file_name(std::string_view segment, int64_t generation) {
    return IndexFileNames::file_name_from_generation(segment, EXTENSION, generation);
}

Result<int32_t> Lucene90LiveDocsFormat::write_bits(DataOutput &out, const Bits &bits) {
    int32_t del_count = 0;
    const size_t long_count = FixedBitSet::bits_to_words(bits.length());
    for (size_t word_index = 0; word_index < long_count; ++word_index) {
        uint64_t current = 0;
        const size_t start = word_index << 6;
        // The last word stops at the real length, so padding bits are never
        // mistaken for documents (they would all count as deletions).
        const size_t end = std::min(start + 63, bits.length() == 0 ? 0 : bits.length() - 1);
        for (size_t bit = start; bit <= end && bits.length() != 0; ++bit) {
            auto value = bits.get(bit);
            if (!value.has_value()) {
                return err(value.error());
            }
            if (value.value()) {
                current |= uint64_t{1} << (bit % 64);
            } else {
                ++del_count;
            }
        }
        if (auto written = out.write_long(static_cast<int64_t>(current)); !written.has_value()) {
            return err(written.error());
        }
    }
    return del_count;
}

Result<FixedBitSet> Lucene90LiveDocsFormat::read_live_docs(const Directory &dir, const SegmentCommitInfo &info,
                                                           const IOContext &context) const {
    if (!info.has_deletions()) {
        return err(LuceneError::illegal_state("segment " + info.info.name + " has no live docs file"));
    }
    const std::string name = file_name(info.info.name, info.del_gen);
    auto input = dir.open_input(name, context);
    if (!input.has_value()) {
        return err(input.error());
    }
    IndexInput &in = *input.value();

    auto result = [&]() -> Result<FixedBitSet> {
        const std::string suffix = IndexFileNames::base36_encode(info.del_gen);
        if (auto checked = CodecUtil::check_index_header(in, CODEC_NAME, VERSION_CURRENT, VERSION_CURRENT,
                                                         info.info.id, suffix);
            !checked.has_value()) {
            return err(checked.error());
        }

        const size_t length = static_cast<size_t>(info.info.max_doc);
        std::vector<uint64_t> words(FixedBitSet::bits_to_words(length), 0);
        for (auto &word : words) {
            auto value = in.read_long();
            if (!value.has_value()) {
                return err(value.error());
            }
            word = static_cast<uint64_t>(value.value());
        }
        auto bits = FixedBitSet::with_capacity(std::move(words), length);
        if (!bits.has_value()) {
            return err(bits.error());
        }
        const size_t deleted = length - bits->cardinality();
        if (deleted != static_cast<size_t>(info.del_count)) {
            return err(LuceneError::corrupt_index("bits.deleted=" + std::to_string(deleted) + " info.delcount=" +
                                                 std::to_string(info.del_count)));
        }
        if (auto footer = CodecUtil::check_footer(in); !footer.has_value()) {
            return err(footer.error());
        }
        return bits;
    }();

    const auto closed = in.close();
    if (!result.has_value()) {
        return result;
    }
    if (!closed.has_value()) {
        return err(closed.error());
    }
    return result;
}

VoidResult Lucene90LiveDocsFormat::write_live_docs(const Bits &bits, Directory &dir, const SegmentCommitInfo &info,
                                                   int32_t new_del_count, const IOContext &context) const {
    if (bits.length() != static_cast<size_t>(info.info.max_doc)) {
        return err(LuceneError::illegal_argument("the live docs length " + std::to_string(bits.length()) +
                                                 " does not match maxDoc=" + std::to_string(info.info.max_doc) +
                                                 " of segment " + info.info.name));
    }

    const int64_t generation = info.next_del_gen();
    const std::string name = file_name(info.info.name, generation);
    auto output = dir.create_output(name, context);
    if (!output.has_value()) {
        return err(output.error());
    }
    IndexOutput &out = *output.value();

    auto written_count = [&]() -> Result<int32_t> {
        const std::string suffix = IndexFileNames::base36_encode(generation);
        if (auto written = CodecUtil::write_index_header(out, CODEC_NAME, VERSION_CURRENT, info.info.id, suffix);
            !written.has_value()) {
            return err(written.error());
        }
        auto del_count = write_bits(out, bits);
        if (!del_count.has_value()) {
            return err(del_count.error());
        }
        if (auto footer = CodecUtil::write_footer(out); !footer.has_value()) {
            return err(footer.error());
        }
        return del_count;
    }();

    const auto closed = out.close();
    if (!written_count.has_value()) {
        (void)dir.delete_file(name);
        return err(written_count.error());
    }
    if (!closed.has_value()) {
        return closed;
    }

    const int32_t expected = info.del_count + new_del_count;
    if (written_count.value() != expected) {
        (void)dir.delete_file(name);
        return err(LuceneError::corrupt_index("bits.deleted=" + std::to_string(written_count.value()) +
                                             " info.delcount=" + std::to_string(info.del_count) +
                                             " newdelcount=" + std::to_string(new_del_count)));
    }
    return {};
}

VoidResult Lucene90LiveDocsFormat::files(const SegmentCommitInfo &info, std::set<std::string> &files) const {
    if (info.has_deletions()) {
        files.insert(file_name(info.info.name, info.del_gen));
    }
    return {};
}

} // namespace lucene
