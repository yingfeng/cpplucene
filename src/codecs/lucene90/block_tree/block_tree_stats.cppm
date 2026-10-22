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

export module lucene_core:block_tree_stats;

import :types;
import std;

import :block_tree_segment_terms_enum;
import :bytes_ref;
import :compression_algorithm;
import :error;

namespace lucene {

/// Block level statistics of one field's term dictionary.
///
/// Port of `Stats` (`core/codecs/lucene90/block_tree/stats.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * `Display` becomes [`to_string`], with the same multi-line layout;
///  * the walk that fills it is [`compute_block_stats`], a free function rather
///    than a method of [`SegmentTermsEnum`]: the enumerator lives in
///    `:block_tree_segment_terms_enum`, which cannot import this partition
///    without a module cycle.
export struct Stats {
    /// Byte size of the index.
    int64_t index_num_bytes{0};

    /// Total number of terms in the field.
    int64_t total_term_count{0};

    /// Total number of bytes (sum of term lengths) across all terms.
    int64_t total_term_bytes{0};

    /// Number of normal (non-floor) blocks in the terms file.
    int32_t non_floor_block_count{0};

    /// Number of floor blocks (meta-blocks larger than the allowed
    /// `maxItemsPerBlock`).
    int32_t floor_block_count{0};

    /// Number of sub-blocks within the floor blocks.
    int32_t floor_sub_block_count{0};

    /// Number of "internal" blocks that have both terms and sub-blocks.
    int32_t mixed_block_count{0};

    /// Number of "leaf" blocks, which only have terms.
    int32_t terms_only_block_count{0};

    /// Number of "internal" blocks without terms, which only have sub-blocks.
    int32_t sub_blocks_only_block_count{0};

    /// Total number of blocks.
    int32_t total_block_count{0};

    /// Number of blocks at each prefix depth.
    std::vector<int32_t> block_count_by_prefix_len{};

    int32_t start_block_count{0};
    int32_t end_block_count{0};

    /// Total number of bytes used to store term suffixes.
    int64_t total_block_suffix_bytes{0};

    /// Number of times each compression method was used, indexed by the on-disk
    /// code (0 = none, 1 = lowercase ASCII, 2 = LZ4).
    std::array<int64_t, 3> compression_algorithms{};

    /// Total number of suffix bytes before compression.
    int64_t total_uncompressed_block_suffix_bytes{0};

    /// Total number of bytes used to store term stats (without the postings
    /// reader's own metadata).
    int64_t total_block_stats_bytes{0};

    /// Total bytes stored by the postings reader and the frame metadata.
    int64_t total_block_other_bytes{0};

    /// Segment name.
    std::string segment;

    /// Field name.
    std::string field;

    [[nodiscard]] static Stats create(std::string segment_name, std::string field_name);

    void start_block(const SegmentTermsEnumFrame &frame, bool is_floor);

    [[nodiscard]] VoidResult end_block(const SegmentTermsEnumFrame &frame);

    void term(std::span<const uint8_t> term_bytes) noexcept { total_term_bytes += static_cast<int64_t>(term_bytes.size()); }

    [[nodiscard]] VoidResult finish() const;

    [[nodiscard]] std::string to_string() const;
};

/// Walks the whole field and computes its block statistics.
///
/// Port of `SegmentTermsEnum::compute_block_stats`; the enumerator is left
/// rewound to the root block, like the reference implementation leaves it.
[[nodiscard]] Result<Stats> compute_block_stats(SegmentTermsEnum &ste);

} // namespace lucene
