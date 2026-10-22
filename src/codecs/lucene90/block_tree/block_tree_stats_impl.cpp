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

module lucene_core:block_tree_stats.impl;

import :block_tree_stats;

import :block_term_state;
import :block_tree_segment_terms_enum;
import :bytes_ref;
import :compression_algorithm;
import :debug;
import :error;
import :fst;
import :terms;
import :types;

import std;

namespace lucene {

Stats Stats::create(std::string segment_name, std::string field_name) {
    Stats stats;
    stats.block_count_by_prefix_len.assign(10, 0);
    stats.compression_algorithms.fill(0);
    stats.segment = std::move(segment_name);
    stats.field = std::move(field_name);
    return stats;
}

void Stats::start_block(const SegmentTermsEnumFrame &frame, bool is_floor) {
    total_block_count += 1;
    if (is_floor) {
        if (frame.fp == frame.fp_orig) {
            floor_block_count += 1;
        }
        floor_sub_block_count += 1;
    } else {
        non_floor_block_count += 1;
    }

    if (block_count_by_prefix_len.size() <= frame.prefix_length) {
        block_count_by_prefix_len.resize(frame.prefix_length + 1, 0);
    }
    block_count_by_prefix_len[frame.prefix_length] += 1;
    start_block_count += 1;
    total_block_suffix_bytes += frame.total_suffix_bytes;
    total_uncompressed_block_suffix_bytes += static_cast<int64_t>(frame.suffixes_reader.length());
    total_uncompressed_block_suffix_bytes += static_cast<int64_t>(frame.suffix_lengths_reader.length());
    total_block_stats_bytes += static_cast<int64_t>(frame.stats_reader.length());
    compression_algorithms[compression_algorithm_code(frame.compression_alg)] += 1;
}

VoidResult Stats::end_block(const SegmentTermsEnumFrame &frame) {
    int32_t term_count = frame.ent_count;
    if (!frame.is_leaf_block) {
        auto block_state = frame.state.get_block_term_state();
        if (!block_state) {
            return std::unexpected(std::move(block_state.error()));
        }
        term_count = block_state.value()->term_block_ord;
    }
    const int32_t sub_block_count = frame.ent_count - term_count;
    total_term_count += term_count;
    if (term_count != 0 && sub_block_count != 0) {
        mixed_block_count += 1;
    } else if (term_count != 0) {
        terms_only_block_count += 1;
    } else if (sub_block_count != 0) {
        sub_blocks_only_block_count += 1;
    } else {
        return err(LuceneError::illegal_state("empty terms block"));
    }
    end_block_count += 1;
    const int64_t other_bytes = static_cast<int64_t>(frame.fp_end) - static_cast<int64_t>(frame.fp) -
                                frame.total_suffix_bytes - static_cast<int64_t>(frame.stats_reader.length());
    debug_check(other_bytes > 0);
    total_block_other_bytes += other_bytes;
    return {};
}

VoidResult Stats::finish() const {
    debug_check(start_block_count == end_block_count);
    debug_check(total_block_count == floor_sub_block_count + non_floor_block_count);
    debug_check(total_block_count == mixed_block_count + terms_only_block_count + sub_blocks_only_block_count);
    return {};
}

std::string Stats::to_string() const {
    std::string out;
    const auto line = [&out](std::string_view text) { out.append(text).push_back('\n'); };

    line("  index FST:");
    line(std::format("    {} bytes", index_num_bytes));

    line("  terms:");
    line(std::format("    {} terms", total_term_count));
    if (total_term_count != 0) {
        line(std::format("    {} bytes ({:.1f} bytes/term)", total_term_bytes,
                         static_cast<double>(total_term_bytes) / static_cast<double>(total_term_count)));
    } else {
        line(std::format("    {} bytes", total_term_bytes));
    }

    line("  blocks:");
    line(std::format("    {} blocks", total_block_count));
    line(std::format("    {} terms-only blocks", terms_only_block_count));
    line(std::format("    {} sub-block-only blocks", sub_blocks_only_block_count));
    line(std::format("    {} mixed blocks", mixed_block_count));
    line(std::format("    {} floor blocks", floor_block_count));
    line(std::format("    {} non-floor blocks", total_block_count - floor_sub_block_count));
    line(std::format("    {} floor sub-blocks", floor_sub_block_count));

    if (total_block_count != 0) {
        line(std::format("    {} term suffix bytes before compression ({:.1f} suffix-bytes/block)",
                         total_uncompressed_block_suffix_bytes,
                         static_cast<double>(total_block_suffix_bytes) / static_cast<double>(total_block_count)));
    } else {
        line(std::format("    {} term suffix bytes before compression", total_uncompressed_block_suffix_bytes));
    }

    std::vector<std::string> compression_summary;
    compression_summary.reserve(compression_algorithms.size());
    for (size_t code = 0; code < compression_algorithms.size(); ++code) {
        const int64_t count = compression_algorithms[code];
        if (count <= 0) {
            continue;
        }
        auto algorithm = compression_algorithm_by_code(static_cast<uint8_t>(code));
        if (!algorithm) {
            line(std::format("    failed to decode compression algorithm code {}: {}", code,
                             algorithm.error().message()));
            return out;
        }
        compression_summary.push_back(std::format("{}: {}", compression_algorithm_name(algorithm.value()), count));
    }
    const double compression_ratio = total_uncompressed_block_suffix_bytes > 0
                                             ? static_cast<double>(total_block_suffix_bytes) /
                                                       static_cast<double>(total_uncompressed_block_suffix_bytes)
                                             : 0.0;
    std::string joined;
    for (size_t i = 0; i < compression_summary.size(); ++i) {
        if (i != 0) {
            joined.append(", ");
        }
        joined.append(compression_summary[i]);
    }
    line(std::format("    {} compressed term suffix bytes ({:.2f} compression ratio - compression count by "
                     "algorithm: {})",
                     total_block_suffix_bytes, compression_ratio, joined));

    if (total_block_count != 0) {
        line(std::format("    {} term stats bytes ({:.1f} stats-bytes/block)", total_block_stats_bytes,
                         static_cast<double>(total_block_stats_bytes) / static_cast<double>(total_block_count)));
    } else {
        line(std::format("    {} term stats bytes", total_block_stats_bytes));
    }

    if (total_block_count != 0) {
        line(std::format("    {} other bytes ({:.1f} other-bytes/block)", total_block_other_bytes,
                         static_cast<double>(total_block_other_bytes) / static_cast<double>(total_block_count)));
    } else {
        line(std::format("    {} other bytes", total_block_other_bytes));
    }

    if (total_block_count != 0) {
        line("    by prefix length:");
        int64_t total = 0;
        for (size_t prefix = 0; prefix < block_count_by_prefix_len.size(); ++prefix) {
            const int32_t count = block_count_by_prefix_len[prefix];
            total += count;
            if (count > 0) {
                line(std::format("      {:2}: {}", prefix, count));
            }
        }
        debug_check(total == total_block_count);
    }
    return out;
}

Result<Stats> compute_block_stats(SegmentTermsEnum &ste) {
    FieldReader &fr = ste.fr();
    if (fr.parent() == nullptr) {
        return err(LuceneError::illegal_state("terms reader is missing"));
    }
    Stats stats = Stats::create(fr.parent()->segment_name(), fr.field_info()->name);
    if (FST *index = fr.index(); index != nullptr) {
        stats.index_num_bytes = index->num_bytes();
    }

    ste.current_frame_idx() = SegmentTermsEnum::STATIC_FRAME_IDX;
    std::optional<size_t> arc;
    if (FST *index = fr.index(); index != nullptr) {
        index->get_first_arc(ste.arcs()[0]);
        debug_check(ste.arcs()[0].is_final());
        arc = 0;
    }
    auto pushed = ste.push_frame_with_data(arc, fr.root_code_, 0);
    if (!pushed) {
        return std::unexpected(std::move(pushed.error()));
    }
    ste.current_frame_idx() = pushed.value();
    ste.stack()[ste.current_frame_idx()].fp_orig = ste.stack()[ste.current_frame_idx()].fp;
    {
        auto loaded = SegmentTermsEnumFrame::load_block(ste.current_frame_idx(), ste);
        if (!loaded) {
            return std::unexpected(std::move(loaded.error()));
        }
    }
    ste.valid_index_prefix() = 0;
    const bool root_is_floor = !ste.stack()[ste.current_frame_idx()].is_last_in_floor;
    stats.start_block(ste.stack()[ste.current_frame_idx()], root_is_floor);

    while (true) {
        while (ste.stack()[ste.current_frame_idx()].next_ent == ste.stack()[ste.current_frame_idx()].ent_count) {
            if (auto ended = stats.end_block(ste.stack()[ste.current_frame_idx()]); !ended) {
                return std::unexpected(std::move(ended.error()));
            }
            if (!ste.stack()[ste.current_frame_idx()].is_last_in_floor) {
                auto loaded = SegmentTermsEnumFrame::load_next_floor_block(ste.current_frame_idx(), ste);
                if (!loaded) {
                    return std::unexpected(std::move(loaded.error()));
                }
                stats.start_block(ste.stack()[ste.current_frame_idx()], true);
                break;
            }
            if (ste.stack()[ste.current_frame_idx()].ord == 0) {
                if (auto finished = stats.finish(); !finished) {
                    return std::unexpected(std::move(finished.error()));
                }
                // Rewind to the root block, as rlucene leaves the enumerator.
                ste.current_frame_idx() = SegmentTermsEnum::STATIC_FRAME_IDX;
                std::optional<size_t> root_arc;
                if (FST *index = fr.index(); index != nullptr) {
                    index->get_first_arc(ste.arcs()[0]);
                    debug_check(ste.arcs()[0].is_final());
                    root_arc = 0;
                }
                auto rewound_push = ste.push_frame_with_data(root_arc, fr.root_code_, 0);
                if (!rewound_push) {
                    return std::unexpected(std::move(rewound_push.error()));
                }
                ste.current_frame_idx() = rewound_push.value();
                {
                    auto rewound = ste.stack()[ste.current_frame_idx()].rewind();
                    if (!rewound) {
                        return std::unexpected(std::move(rewound.error()));
                    }
                }
                auto reloaded = SegmentTermsEnumFrame::load_block(ste.current_frame_idx(), ste);
                if (!reloaded) {
                    return std::unexpected(std::move(reloaded.error()));
                }
                ste.valid_index_prefix() = 0;
                ste.term_builder().clear();
                return stats;
            }
            const size_t last_fp = ste.stack()[ste.current_frame_idx()].fp_orig;
            ste.current_frame_idx() = static_cast<size_t>(ste.stack()[ste.current_frame_idx()].ord);
            debug_check(ste.stack()[ste.current_frame_idx()].last_sub_fp.has_value() &&
                        ste.stack()[ste.current_frame_idx()].last_sub_fp.value() == last_fp);
        }

        while (true) {
            auto has_next = SegmentTermsEnumFrame::next(ste.current_frame_idx(), ste);
            if (!has_next) {
                return std::unexpected(std::move(has_next.error()));
            }
            if (!has_next.value()) {
                stats.term(ste.term_bytes());
                break;
            }
            const auto last_sub_fp = ste.stack()[ste.current_frame_idx()].last_sub_fp;
            if (!last_sub_fp.has_value()) {
                return err(LuceneError::illegal_state("term block has no current sub-block"));
            }
            auto deeper = ste.push_frame(std::nullopt, last_sub_fp.value(), ste.term_builder().length());
            if (!deeper) {
                return std::unexpected(std::move(deeper.error()));
            }
            ste.current_frame_idx() = deeper.value();
            ste.stack()[ste.current_frame_idx()].fp_orig = ste.stack()[ste.current_frame_idx()].fp;
            {
                auto loaded = SegmentTermsEnumFrame::load_block(ste.current_frame_idx(), ste);
                if (!loaded) {
                    return std::unexpected(std::move(loaded.error()));
                }
            }
            const bool is_floor = !ste.stack()[ste.current_frame_idx()].is_last_in_floor;
            stats.start_block(ste.stack()[ste.current_frame_idx()], is_floor);
        }
    }
}

// `FieldReader::get_stats` lives here: rlucene implements it in terms of
// `compute_block_stats`, and this partition is the one that can see both.
Result<std::string> FieldReader::get_stats() const {
    auto copy = clone_for_enum();
    if (!copy) {
        return std::unexpected(std::move(copy.error()));
    }
    auto enumerator = SegmentTermsEnum::create(std::move(copy.value()));
    if (!enumerator) {
        return std::unexpected(std::move(enumerator.error()));
    }
    auto stats = compute_block_stats(*enumerator.value());
    if (!stats) {
        return std::unexpected(std::move(stats.error()));
    }
    return stats.value().to_string();
}

} // namespace lucene
