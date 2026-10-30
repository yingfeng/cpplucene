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

module lucene_core:lucene99_segment_info_format.impl;

import :lucene99_segment_info_format;

import :codec_util;
import :data_input;
import :data_output;
import :directory;
import :error;
import :index_input;
import :index_output;
import :io_context;
import :segment_info;
import :segment_info_format;
import :sort_field;
import :sort_field_provider;
import :types;
import :version;

import std;

namespace lucene {

namespace {

constexpr uint8_t YES = 1;
constexpr uint8_t NO = 0;

/// Writes one version component triplet.
///
/// The version components are written with the plain (little-endian)
/// `DataOutput::write_int`, like the reference implementation, which uses
/// `writeInt` here.  `CodecUtil::write_be_int` is reserved for the codec and
/// footer magic numbers only.
VoidResult write_version(DataOutput &out, const Version &version) {
    if (auto written = out.write_int(version.major); !written.has_value()) {
        return written;
    }
    if (auto written = out.write_int(version.minor); !written.has_value()) {
        return written;
    }
    return out.write_int(version.bug_fix);
}

Result<Version> read_version(DataInput &input) {
    auto major = input.read_int();
    if (!major.has_value()) {
        return err(major.error());
    }
    auto minor = input.read_int();
    if (!minor.has_value()) {
        return err(minor.error());
    }
    auto bug_fix = input.read_int();
    if (!bug_fix.has_value()) {
        return err(bug_fix.error());
    }
    return Version::from_bits(major.value(), minor.value(), bug_fix.value());
}

} // namespace

VoidResult Lucene99SegmentInfoFormat::write_segment_info(DataOutput &out, const SegmentInfo &info) {
    if (!info.version.has_value()) {
        return err(LuceneError::illegal_state("the segment version is missing"));
    }
    if (info.version->major < 7) {
        return err(LuceneError::illegal_argument("the segment version must be at least 7.0.0, got " +
                                                 info.version->to_string()));
    }
    if (auto written = write_version(out, info.version.value()); !written.has_value()) {
        return written;
    }

    if (info.min_version.has_value()) {
        if (auto written = out.write_byte(YES); !written.has_value()) {
            return written;
        }
        if (auto written = write_version(out, info.min_version.value()); !written.has_value()) {
            return written;
        }
    } else if (auto written = out.write_byte(NO); !written.has_value()) {
        return written;
    }

    if (auto written = out.write_int(info.max_doc); !written.has_value()) {
        return written;
    }
    if (auto written = out.write_byte(info.use_compound_file ? YES : NO); !written.has_value()) {
        return written;
    }
    if (auto written = out.write_byte(info.has_blocks ? YES : NO); !written.has_value()) {
        return written;
    }
    if (auto written = out.write_map_of_strings(info.diagnostics); !written.has_value()) {
        return written;
    }

    // Every file must belong to this segment: a `.si` that points at another
    // segment's files is the classic symptom of a half-copied index.
    for (const auto &file : info.files) {
        if (auto checked = check_file_name(file, info.name); !checked.has_value()) {
            return err(LuceneError::illegal_argument("invalid file for segment " + info.name + ": " + file));
        }
    }
    if (auto written = out.write_set_of_strings(info.files); !written.has_value()) {
        return written;
    }
    if (auto written = out.write_map_of_strings(info.attributes); !written.has_value()) {
        return written;
    }

    const std::vector<SortField> *sort_fields = info.sort.has_value() ? &info.sort.value().fields() : nullptr;
    const size_t num_sort_fields = sort_fields == nullptr ? 0 : sort_fields->size();
    if (auto written = out.write_vint(static_cast<int32_t>(num_sort_fields)); !written.has_value()) {
        return written;
    }
    if (sort_fields == nullptr) {
        return {};
    }
    for (const SortField &field : *sort_fields) {
        // Only a field that can drive an index sort may be recorded; the same
        // check the reference implementation does through `get_index_sorter`.
        auto sorter = field.index_sorter();
        if (!sorter.has_value()) {
            auto text = field.to_string();
            return err(LuceneError::illegal_argument(
                    "cannot serialize sort field " + (text.has_value() ? text.value() : std::string("?"))));
        }
        auto provider = sort_field_provider_name(field);
        if (!provider.has_value()) {
            return err(provider.error());
        }
        if (auto written = out.write_string(provider.value()); !written.has_value()) {
            return written;
        }
        if (auto written = write_sort_field(field, out); !written.has_value()) {
            return written;
        }
    }
    return {};
}

Result<SegmentInfo> Lucene99SegmentInfoFormat::parse_segment_info(DataInput &input, std::string_view segment,
                                                                 std::span<const uint8_t> id) {
    SegmentInfo info;
    info.name = std::string(segment);
    info.id.assign(id.begin(), id.end());

    auto version = read_version(input);
    if (!version.has_value()) {
        return err(version.error());
    }
    info.version = version.value();

    auto has_min_version = input.read_byte();
    if (!has_min_version.has_value()) {
        return err(has_min_version.error());
    }
    if (has_min_version.value() == YES) {
        auto min_version = read_version(input);
        if (!min_version.has_value()) {
            return err(min_version.error());
        }
        info.min_version = min_version.value();
    } else if (has_min_version.value() != NO) {
        return err(LuceneError::corrupt_index("invalid bool value: " + std::to_string(has_min_version.value()) +
                                              " (resource=" + std::string(input.to_string()) + ")"));
    }

    auto max_doc = input.read_int();
    if (!max_doc.has_value()) {
        return err(max_doc.error());
    }
    if (max_doc.value() < 0) {
        return err(LuceneError::corrupt_index("invalid docCount: " + std::to_string(max_doc.value()) +
                                              " (resource=" + std::string(input.to_string()) + ")"));
    }
    info.max_doc = max_doc.value();

    auto compound = input.read_byte();
    if (!compound.has_value()) {
        return err(compound.error());
    }
    info.use_compound_file = compound.value() == YES;

    auto blocks = input.read_byte();
    if (!blocks.has_value()) {
        return err(blocks.error());
    }
    info.has_blocks = blocks.value() == YES;

    auto diagnostics = input.read_map_of_strings();
    if (!diagnostics.has_value()) {
        return err(diagnostics.error());
    }
    info.diagnostics = std::move(diagnostics.value());

    auto files = input.read_set_of_strings();
    if (!files.has_value()) {
        return err(files.error());
    }
    if (auto assigned = info.set_files(std::move(files.value())); !assigned.has_value()) {
        return err(assigned.error());
    }

    auto attributes = input.read_map_of_strings();
    if (!attributes.has_value()) {
        return err(attributes.error());
    }
    info.attributes = std::move(attributes.value());

    auto num_sort_fields = input.read_vint();
    if (!num_sort_fields.has_value()) {
        return err(num_sort_fields.error());
    }
    if (num_sort_fields.value() < 0) {
        return err(LuceneError::corrupt_index("invalid index sort field count: " +
                                              std::to_string(num_sort_fields.value()) + " (resource=" +
                                              std::string(input.to_string()) + ")"));
    }
    if (num_sort_fields.value() > 0) {
        std::vector<SortField> sort_fields;
        sort_fields.reserve(static_cast<size_t>(num_sort_fields.value()));
        for (int32_t i = 0; i < num_sort_fields.value(); ++i) {
            auto provider_name = input.read_string();
            if (!provider_name.has_value()) {
                return err(provider_name.error());
            }
            auto field = read_sort_field(provider_name.value(), input);
            if (!field.has_value()) {
                return err(field.error());
            }
            sort_fields.push_back(std::move(field.value()));
        }
        auto sort = Sort::with_fields(std::move(sort_fields));
        if (!sort.has_value()) {
            return err(sort.error());
        }
        info.sort = std::move(sort.value());
    }
    return info;
}

VoidResult Lucene99SegmentInfoFormat::write(Directory &dir, SegmentInfo &info, const IOContext &context) const {
    if (auto valid = info.validate(); !valid.has_value()) {
        return valid;
    }

    const std::string file_name = info.si_file_name();
    auto output = dir.create_output(file_name, context);
    if (!output.has_value()) {
        return err(output.error());
    }
    IndexOutput &out = *output.value();

    if (auto added = info.add_file(file_name); !added.has_value()) {
        (void)out.close();
        return added;
    }

    auto result = [&]() -> VoidResult {
        if (auto written = CodecUtil::write_index_header(out, CODEC_NAME, VERSION_CURRENT, info.id, "");
            !written.has_value()) {
            return written;
        }
        if (auto written = write_segment_info(out, info); !written.has_value()) {
            return written;
        }
        return CodecUtil::write_footer(out);
    }();

    const auto closed = out.close();
    if (!result.has_value()) {
        // A half written `.si` is worse than no file at all: the commit point that
        // was supposed to reference it does not exist yet, so remove the debris.
        (void)dir.delete_file(file_name);
        return result;
    }
    return closed;
}

Result<SegmentInfo> Lucene99SegmentInfoFormat::read(const Directory &dir, std::string_view segment,
                                                    std::span<const uint8_t> id, const IOContext &context) const {
    const std::string file_name = segment_file_name(segment, "", SI_EXTENSION);
    auto input = dir.open_input(file_name, context);
    if (!input.has_value()) {
        return err(input.error());
    }
    IndexInput &in = *input.value();

    auto result = [&]() -> Result<SegmentInfo> {
        if (auto checked = CodecUtil::check_index_header(in, CODEC_NAME, VERSION_CURRENT, VERSION_CURRENT, id, "");
            !checked.has_value()) {
            return err(checked.error());
        }
        auto parsed = parse_segment_info(in, segment, id);
        if (!parsed.has_value()) {
            return err(parsed.error());
        }
        if (auto footer = CodecUtil::check_footer(in); !footer.has_value()) {
            return err(footer.error());
        }
        return parsed;
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

} // namespace lucene
