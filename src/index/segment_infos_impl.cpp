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

module lucene_core:segment_infos.impl;

import :segment_infos;

import :codec;
import :codec_util;
import :data_input;
import :data_output;
import :directory;
import :error;
import :index_file_names;
import :index_input;
import :index_output;
import :io_context;
import :lucene99_codec;
import :segment_commit_info;
import :segment_info;
import :segment_info_format;
import :types;
import :version;

import std;

namespace lucene {

namespace {

/// The codec name recorded in every commit file header.
constexpr std::string_view SEGMENTS_CODEC = "segments";

/// Sixteen bytes for the header identity.
std::vector<uint8_t> fresh_id() {
    std::random_device device;
    std::mt19937_64 engine((static_cast<uint64_t>(device()) << 32) ^ device());
    std::vector<uint8_t> id(16, 0);
    for (size_t i = 0; i < id.size(); i += 8) {
        const uint64_t chunk = engine();
        for (size_t byte = 0; byte < 8; ++byte) {
            id[i + byte] = static_cast<uint8_t>((chunk >> (8 * byte)) & 0xFF);
        }
    }
    return id;
}

Result<Version> read_vint_version(DataInput &input) {
    auto major = input.read_vint();
    if (!major.has_value()) {
        return err(major.error());
    }
    auto minor = input.read_vint();
    if (!minor.has_value()) {
        return err(minor.error());
    }
    auto bug_fix = input.read_vint();
    if (!bug_fix.has_value()) {
        return err(bug_fix.error());
    }
    return Version::from_bits(major.value(), minor.value(), bug_fix.value());
}

VoidResult write_vint_version(DataOutput &out, const Version &version) {
    if (auto written = out.write_vint(version.major); !written.has_value()) {
        return written;
    }
    if (auto written = out.write_vint(version.minor); !written.has_value()) {
        return written;
    }
    return out.write_vint(version.bug_fix);
}

} // namespace

int64_t SegmentInfos::total_max_doc() const {
    int64_t total = 0;
    for (const auto &segment : segments) {
        total += segment.info.max_doc;
    }
    return total;
}

VoidResult SegmentInfos::add(SegmentCommitInfo segment) {
    if (auto valid = segment.validate(); !valid.has_value()) {
        return valid;
    }
    segments.push_back(std::move(segment));
    return {};
}

std::string SegmentInfos::generation_file_name() const {
    return IndexFileNames::file_name_from_generation(IndexFileNames::SEGMENTS, "", generation);
}

VoidResult SegmentInfos::write(IndexOutput &out) const {
    if (index_created_version_major < LUCENE_MIN_SUPPORTED_VERSION.major) {
        return err(LuceneError::illegal_state("cannot write an index created by Lucene " +
                                              std::to_string(index_created_version_major) +
                                              ".x: the minimum supported major version is " +
                                              std::to_string(LUCENE_MIN_SUPPORTED_VERSION.major)));
    }

    std::vector<uint8_t> header_id = id;
    if (header_id.size() != ID_LENGTH) {
        // rlucene writes a random id here; this port prefers the index id so the
        // file is reproducible, and only falls back to random bytes when the
        // caller never assigned one.
        header_id = fresh_id();
    }
    const std::string suffix = IndexFileNames::base36_encode(generation);
    if (auto written = CodecUtil::write_index_header(out, SEGMENTS_CODEC, VERSION_CURRENT, header_id, suffix);
        !written.has_value()) {
        return written;
    }

    if (auto written = write_vint_version(out, lucene_version); !written.has_value()) {
        return written;
    }
    if (auto written = out.write_vint(index_created_version_major); !written.has_value()) {
        return written;
    }
    if (auto written = CodecUtil::write_be_long(out, version); !written.has_value()) {
        return written;
    }
    if (auto written = out.write_vlong(counter); !written.has_value()) {
        return written;
    }
    if (auto written = CodecUtil::write_be_int(out, static_cast<int32_t>(segments.size())); !written.has_value()) {
        return written;
    }

    if (!segments.empty()) {
        // The minimum version is written up front so a reader can reject a whole
        // index before touching any segment.
        std::optional<Version> minimum;
        for (const auto &segment : segments) {
            if (!segment.info.version.has_value()) {
                return err(LuceneError::illegal_state("segment " + segment.info.name +
                                                      " does not record a Lucene version"));
            }
            if (!minimum.has_value() || *segment.info.version < *minimum) {
                minimum = segment.info.version;
            }
        }
        if (auto written = write_vint_version(out, minimum.value()); !written.has_value()) {
            return written;
        }
    }

    for (const auto &segment : segments) {
        const SegmentInfo &info = segment.info;
        if (index_created_version_major >= 7 && !info.min_version.has_value()) {
            return err(LuceneError::illegal_state("segment " + info.name +
                                                  " must record a minVersion, the index was created with Lucene "
                                                  "7.0 or later"));
        }
        if (auto written = out.write_string(info.name); !written.has_value()) {
            return written;
        }
        if (info.id.size() != ID_LENGTH) {
            return err(LuceneError::illegal_state("invalid id for segment " + info.name + ": expected " +
                                                  std::to_string(ID_LENGTH) + " bytes, got " +
                                                  std::to_string(info.id.size())));
        }
        if (auto written = out.write_bytes(info.id); !written.has_value()) {
            return written;
        }
        if (auto written = out.write_string(info.codec_name); !written.has_value()) {
            return written;
        }

        if (auto written = CodecUtil::write_be_long(out, segment.del_gen); !written.has_value()) {
            return written;
        }
        if (auto written = CodecUtil::write_be_int(out, segment.del_count); !written.has_value()) {
            return written;
        }
        if (auto written = CodecUtil::write_be_long(out, segment.field_infos_gen); !written.has_value()) {
            return written;
        }
        if (auto written = CodecUtil::write_be_long(out, segment.doc_values_gen); !written.has_value()) {
            return written;
        }
        if (auto written = CodecUtil::write_be_int(out, segment.soft_del_count); !written.has_value()) {
            return written;
        }

        if (segment.id.has_value()) {
            if (segment.id->size() != ID_LENGTH) {
                return err(LuceneError::illegal_state("invalid commit id for segment " + info.name));
            }
            if (auto written = out.write_byte(1); !written.has_value()) {
                return written;
            }
            if (auto written = out.write_bytes(segment.id.value()); !written.has_value()) {
                return written;
            }
        } else if (auto written = out.write_byte(0); !written.has_value()) {
            return written;
        }

        if (auto written = out.write_set_of_strings(segment.field_infos_files); !written.has_value()) {
            return written;
        }
        if (auto written = CodecUtil::write_be_int(out, static_cast<int32_t>(segment.doc_values_updates_files.size()));
            !written.has_value()) {
            return written;
        }
        for (const auto &[generation_key, files] : segment.doc_values_updates_files) {
            if (auto written = CodecUtil::write_be_int(out, generation_key); !written.has_value()) {
                return written;
            }
            if (auto written = out.write_set_of_strings(files); !written.has_value()) {
                return written;
            }
        }
    }

    if (auto written = out.write_map_of_strings(user_data); !written.has_value()) {
        return written;
    }
    return CodecUtil::write_footer(out);
}

VoidResult SegmentInfos::write_to(Directory &dir, const IOContext &context) const {
    const std::string file_name = generation_file_name();
    auto output = dir.create_output(file_name, context);
    if (!output.has_value()) {
        return err(output.error());
    }
    IndexOutput &out = *output.value();

    auto result = write(out);
    const auto closed = out.close();
    if (!result.has_value()) {
        (void)dir.delete_file(file_name);
        return result;
    }
    return closed;
}

Result<SegmentInfos> SegmentInfos::read_file(const Directory &dir, std::string_view file_name, int64_t generation,
                                             const IOContext &context) {
    auto input = dir.open_input(file_name, context);
    if (!input.has_value()) {
        return err(input.error());
    }
    IndexInput &in = *input.value();

    auto result = [&]() -> Result<SegmentInfos> {
        // The magic is read separately so an old index reports
        // "format too old" instead of "corrupt".
        auto magic = CodecUtil::read_be_int(in);
        if (!magic.has_value()) {
            return err(magic.error());
        }
        if (magic.value() != CodecUtil::CODEC_MAGIC) {
            return err(LuceneError(ErrorKind::kIndexFormatTooOld,
                                   "codec header mismatch: actual header=" + std::to_string(magic.value()) +
                                       " vs expected header=" + std::to_string(CodecUtil::CODEC_MAGIC)));
        }
        if (auto format = CodecUtil::check_header_no_magic(in, SEGMENTS_CODEC, VERSION_CURRENT, VERSION_CURRENT);
            !format.has_value()) {
            return err(format.error());
        }

        std::vector<uint8_t> header_id(ID_LENGTH, 0);
        if (auto read = in.read_bytes(header_id); !read.has_value()) {
            return err(read.error());
        }
        auto suffix = in.read_string();
        if (!suffix.has_value()) {
            return err(suffix.error());
        }
        const std::string expected_suffix = IndexFileNames::base36_encode(generation);
        if (suffix.value() != expected_suffix) {
            return err(LuceneError::corrupt_index("segments file suffix mismatch: expected '" + expected_suffix +
                                                  "', got '" + suffix.value() + "'"));
        }

        SegmentInfos infos;
        infos.id = std::move(header_id);
        infos.generation = generation;
        infos.last_generation = generation;

        auto lucene_version = read_vint_version(in);
        if (!lucene_version.has_value()) {
            return err(lucene_version.error());
        }
        infos.lucene_version = lucene_version.value();

        auto created_major = in.read_vint();
        if (!created_major.has_value()) {
            return err(created_major.error());
        }
        if (created_major.value() < 0) {
            return err(LuceneError::corrupt_index("invalid indexCreatedVersionMajor: " +
                                                  std::to_string(created_major.value())));
        }
        infos.index_created_version_major = created_major.value();
        if (infos.lucene_version.major < infos.index_created_version_major) {
            return err(LuceneError::corrupt_index(
                "creation version " + std::to_string(infos.index_created_version_major) +
                " cannot be greater than the version that wrote the segment infos: " +
                infos.lucene_version.to_string()));
        }
        if (infos.index_created_version_major < LUCENE_MIN_SUPPORTED_VERSION.major) {
            return err(LuceneError(ErrorKind::kIndexFormatTooOld,
                                   "this index was created with Lucene " +
                                       std::to_string(infos.index_created_version_major) +
                                       ".x while the minimum supported major version is " +
                                       std::to_string(LUCENE_MIN_SUPPORTED_VERSION.major)));
        }

        auto version = CodecUtil::read_be_long(in);
        if (!version.has_value()) {
            return err(version.error());
        }
        infos.version = version.value();

        auto counter = in.read_vlong();
        if (!counter.has_value()) {
            return err(counter.error());
        }
        if (counter.value() < 0) {
            return err(LuceneError::corrupt_index("invalid segment counter: " + std::to_string(counter.value())));
        }
        infos.counter = counter.value();

        auto num_segments = CodecUtil::read_be_int(in);
        if (!num_segments.has_value()) {
            return err(num_segments.error());
        }
        if (num_segments.value() < 0) {
            return err(LuceneError::corrupt_index("invalid segment count: " + std::to_string(num_segments.value())));
        }
        if (num_segments.value() > 0) {
            auto min_version = read_vint_version(in);
            if (!min_version.has_value()) {
                return err(min_version.error());
            }
            infos.min_segment_lucene_version = min_version.value();
        }

        for (int32_t i = 0; i < num_segments.value(); ++i) {
            auto name = in.read_string();
            if (!name.has_value()) {
                return err(name.error());
            }
            std::vector<uint8_t> segment_id(ID_LENGTH, 0);
            if (auto read = in.read_bytes(segment_id); !read.has_value()) {
                return err(read.error());
            }
            auto codec_name = in.read_string();
            if (!codec_name.has_value()) {
                return err(codec_name.error());
            }
            auto codec = codec_for_name(codec_name.value());
            if (!codec.has_value()) {
                return err(codec.error());
            }

            auto segment_info = codec.value()->segment_info_format().read(dir, name.value(), segment_id, context);
            if (!segment_info.has_value()) {
                return err(segment_info.error());
            }

            SegmentCommitInfo segment;
            segment.info = std::move(segment_info.value());

            auto del_gen = CodecUtil::read_be_long(in);
            if (!del_gen.has_value()) {
                return err(del_gen.error());
            }
            segment.del_gen = del_gen.value();

            auto del_count = CodecUtil::read_be_int(in);
            if (!del_count.has_value()) {
                return err(del_count.error());
            }
            segment.del_count = del_count.value();
            if (segment.del_count < 0 || segment.del_count > segment.info.max_doc) {
                return err(LuceneError::corrupt_index("invalid deletion count: " + std::to_string(segment.del_count) +
                                                      " vs maxDoc=" + std::to_string(segment.info.max_doc)));
            }

            auto field_infos_gen = CodecUtil::read_be_long(in);
            if (!field_infos_gen.has_value()) {
                return err(field_infos_gen.error());
            }
            segment.field_infos_gen = field_infos_gen.value();

            auto doc_values_gen = CodecUtil::read_be_long(in);
            if (!doc_values_gen.has_value()) {
                return err(doc_values_gen.error());
            }
            segment.doc_values_gen = doc_values_gen.value();

            auto soft_del_count = CodecUtil::read_be_int(in);
            if (!soft_del_count.has_value()) {
                return err(soft_del_count.error());
            }
            segment.soft_del_count = soft_del_count.value();
            if (segment.soft_del_count < 0 || segment.soft_del_count > segment.info.max_doc) {
                return err(LuceneError::corrupt_index("invalid soft deletion count: " +
                                                      std::to_string(segment.soft_del_count) +
                                                      " vs maxDoc=" + std::to_string(segment.info.max_doc)));
            }
            if (segment.total_del_count() > segment.info.max_doc) {
                return err(LuceneError::corrupt_index("invalid combined deletion count: " +
                                                      std::to_string(segment.total_del_count()) +
                                                      " vs maxDoc=" + std::to_string(segment.info.max_doc)));
            }

            auto marker = in.read_byte();
            if (!marker.has_value()) {
                return err(marker.error());
            }
            if (marker.value() == 1) {
                std::vector<uint8_t> commit_id(ID_LENGTH, 0);
                if (auto read = in.read_bytes(commit_id); !read.has_value()) {
                    return err(read.error());
                }
                segment.id = std::move(commit_id);
            } else if (marker.value() != 0) {
                return err(LuceneError::corrupt_index("invalid SegmentCommitInfo id marker: " +
                                                      std::to_string(marker.value())));
            }

            auto field_infos_files = in.read_set_of_strings();
            if (!field_infos_files.has_value()) {
                return err(field_infos_files.error());
            }
            segment.field_infos_files = std::move(field_infos_files.value());

            auto dv_update_count = CodecUtil::read_be_int(in);
            if (!dv_update_count.has_value()) {
                return err(dv_update_count.error());
            }
            if (dv_update_count.value() < 0) {
                return err(LuceneError::corrupt_index("invalid doc values update count: " +
                                                      std::to_string(dv_update_count.value())));
            }
            for (int32_t entry = 0; entry < dv_update_count.value(); ++entry) {
                auto generation_key = CodecUtil::read_be_int(in);
                if (!generation_key.has_value()) {
                    return err(generation_key.error());
                }
                auto files = in.read_set_of_strings();
                if (!files.has_value()) {
                    return err(files.error());
                }
                segment.doc_values_updates_files.emplace(generation_key.value(), std::move(files.value()));
            }

            if (!segment.info.version.has_value() || !infos.min_segment_lucene_version.has_value()) {
                return err(LuceneError::corrupt_index("segment " + segment.info.name +
                                                      " or the commit file does not record a Lucene version"));
            }
            if (*segment.info.version < *infos.min_segment_lucene_version) {
                return err(LuceneError::corrupt_index("the commit file records minSegmentLuceneVersion=" +
                                                      infos.min_segment_lucene_version->to_string() +
                                                      " but segment " + segment.info.name + " has version=" +
                                                      segment.info.version->to_string()));
            }
            if (segment.info.version->major < infos.index_created_version_major) {
                return err(LuceneError::corrupt_index("the commit file records indexCreatedVersionMajor=" +
                                                      std::to_string(infos.index_created_version_major) +
                                                      " but segment " + segment.info.name + " has version=" +
                                                      segment.info.version->to_string()));
            }

            infos.segments.push_back(std::move(segment));
        }

        auto user_data = in.read_map_of_strings();
        if (!user_data.has_value()) {
            return err(user_data.error());
        }
        infos.user_data = std::move(user_data.value());

        if (infos.total_max_doc() > MAX_DOCS) {
            return err(LuceneError::corrupt_index("too many documents: an index cannot exceed " +
                                                  std::to_string(MAX_DOCS) + " but readers have total maxDoc=" +
                                                  std::to_string(infos.total_max_doc())));
        }

        if (auto footer = CodecUtil::check_footer(in); !footer.has_value()) {
            return err(footer.error());
        }
        return infos;
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

Result<SegmentInfos> SegmentInfos::read(const Directory &dir, int64_t generation, const IOContext &context) {
    const std::string file_name =
        IndexFileNames::file_name_from_generation(IndexFileNames::SEGMENTS, "", generation);
    return read_file(dir, file_name, generation, context);
}

Result<std::vector<int64_t>> SegmentInfos::list_generations(const Directory &dir) {
    auto names = dir.list_all();
    if (!names.has_value()) {
        return err(names.error());
    }
    std::vector<int64_t> generations;
    for (const auto &name : names.value()) {
        if (!name.starts_with(IndexFileNames::SEGMENTS) || name == IndexFileNames::OLD_SEGMENTS_GEN) {
            continue;
        }
        auto generation = IndexFileNames::generation_from_segment_file_name(name, IndexFileNames::SEGMENTS);
        if (!generation.has_value()) {
            // Not a commit file: `segments.gen` and friends are ignored.
            continue;
        }
        generations.push_back(generation.value());
    }
    std::sort(generations.begin(), generations.end());
    return generations;
}

Result<SegmentInfos> SegmentInfos::read_latest(const Directory &dir, const IOContext &context) {
    auto generations = list_generations(dir);
    if (!generations.has_value()) {
        return err(generations.error());
    }
    if (generations->empty()) {
        return err(LuceneError::index_not_found("no segments_N file in this directory"));
    }
    // Generations are compared numerically, not lexicographically: "a" > "9" as
    // text but base 36 "a" == 10, and "10" comes before "9" as text.
    return read(dir, generations->back(), context);
}

} // namespace lucene
