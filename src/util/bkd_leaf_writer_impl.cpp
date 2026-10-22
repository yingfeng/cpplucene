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

module lucene_core:bkd_leaf_writer.impl;

import :bkd_leaf_writer;

import :bkd_config;
import :bkd_doc_ids_writer;
import :bkd_heap_point_writer;
import :bkd_tree_planner;
import :bkd_util;
import :data_output;
import :error;
import :types;

import std;

namespace lucene {

namespace {

/// Byte that marks "every value in this block is identical".
constexpr uint8_t kAllEqual = 0xFF;
/// Byte that marks the low-cardinality encoding.
constexpr uint8_t kLowCardinality = 0xFE;

/// Writes the stored part (that is, past the common prefix) of every dimension.
[[nodiscard]] VoidResult write_suffixes(DataOutput &out, std::span<const uint8_t> value,
                                        std::span<const size_t> prefixes, size_t num_dims, size_t bytes_per_dim) {
    for (size_t dim = 0; dim < num_dims; ++dim) {
        const size_t prefix = prefixes[dim];
        const size_t length = bytes_per_dim - prefix;
        if (length == 0) {
            continue;
        }
        if (auto written = out.write_bytes(value.subspan(dim * bytes_per_dim + prefix, length));
            !written.has_value()) {
            return written;
        }
    }
    return {};
}

/// Whether two packed values agree on every stored part.
///
/// The common prefix bytes are identical by construction, so only the suffix
/// decides whether two points are the same value.
[[nodiscard]] bool suffix_equals(std::span<const uint8_t> a, std::span<const uint8_t> b,
                                 std::span<const size_t> prefixes, size_t num_dims, size_t bytes_per_dim) {
    for (size_t dim = 0; dim < num_dims; ++dim) {
        const size_t prefix = prefixes[dim];
        const size_t length = bytes_per_dim - prefix;
        if (length == 0) {
            continue;
        }
        const size_t offset = dim * bytes_per_dim + prefix;
        if (BKDUtil::compare_unsigned(a, offset, b, offset, length) != 0) {
            return false;
        }
    }
    return true;
}

/// Picks the byte to run-length compress.
///
/// A dimension whose common prefix already covers the whole dimension has no byte
/// left to compress in place, so it cannot be used; the leaf's own sort dimension
/// is preferred because it groups neighbouring points, but any other usable
/// dimension is a valid fallback (the encoder stores the chosen byte per run, so
/// correctness does not depend on the choice).
[[nodiscard]] size_t pick_sorted_dim(size_t preferred, std::span<const size_t> prefixes, size_t num_dims,
                                    size_t bytes_per_dim) {
    if (preferred < num_dims && prefixes[preferred] < bytes_per_dim) {
        return preferred;
    }
    for (size_t dim = 0; dim < num_dims; ++dim) {
        if (prefixes[dim] < bytes_per_dim) {
            return dim;
        }
    }
    return 0;
}

/// Writes the min and max suffix of every indexed dimension.
///
/// rlucene only does this when more than one dimension is indexed: with a
/// single indexed dimension the common prefix already pins the block down well
/// enough for the reader, and the extra bytes would be pure overhead.
[[nodiscard]] VoidResult write_actual_bounds(HeapPointWriter &points, const BkdLeaf &leaf,
                                             std::span<const size_t> prefixes, size_t num_index_dims,
                                             size_t bytes_per_dim, DataOutput &out) {
    for (size_t dim = 0; dim < num_index_dims; ++dim) {
        const size_t prefix = prefixes[dim];
        const size_t length = bytes_per_dim - prefix;
        if (length == 0) {
            continue;
        }
        const size_t offset = dim * bytes_per_dim + prefix;

        const auto first = points.packed_value(leaf.from).packed_value();
        std::vector<uint8_t> min_suffix(first.begin() + static_cast<std::ptrdiff_t>(offset),
                                        first.begin() + static_cast<std::ptrdiff_t>(offset + length));
        std::vector<uint8_t> max_suffix(min_suffix);
        for (size_t i = leaf.from + 1; i < leaf.to; ++i) {
            const auto candidate = points.packed_value(i).packed_value().subspan(offset, length);
            if (BKDUtil::compare_unsigned(candidate, 0, min_suffix, 0, length) < 0) {
                std::copy(candidate.begin(), candidate.end(), min_suffix.begin());
            } else if (BKDUtil::compare_unsigned(candidate, 0, max_suffix, 0, length) > 0) {
                std::copy(candidate.begin(), candidate.end(), max_suffix.begin());
            }
        }
        if (auto written = out.write_bytes(min_suffix); !written.has_value()) {
            return written;
        }
        if (auto written = out.write_bytes(max_suffix); !written.has_value()) {
            return written;
        }
    }
    return {};
}

} // namespace

Result<BkdLeafBlockInfo> BkdLeafWriter::write_leaf_block(HeapPointWriter &points, const BkdLeaf &leaf,
                                                        DataOutput &out) {
    if (!points.closed()) {
        return err(LuceneError::illegal_state("the point writer must be closed before writing leaf blocks"));
    }
    if (leaf.count() == 0 || leaf.to > points.count()) {
        return err(LuceneError::illegal_argument("the leaf range must be non-empty and inside the writer"));
    }

    const auto &config = points.config();
    const size_t count = leaf.count();
    const size_t num_dims = config.num_dims;
    const size_t bytes_per_dim = config.bytes_per_dim;

    BkdLeafBlockInfo info;
    info.count = count;

    // ---- doc ids ----
    if (auto written = out.write_vint(static_cast<int32_t>(count)); !written.has_value()) {
        return err(written.error());
    }
    std::vector<int32_t> doc_ids(count, 0);
    for (size_t i = 0; i < count; ++i) {
        doc_ids[i] = points.packed_value(leaf.from + i).doc_id();
    }
    if (auto written = BkdDocIdsWriter::write_doc_ids(doc_ids, out); !written.has_value()) {
        return err(written.error());
    }

    // ---- per dimension common prefixes ----
    // Scanned over the whole block rather than derived from the first and last
    // value: the leaf is sorted on a single dimension, so its extremes do not
    // bracket the other ones and their prefix would silently truncate values
    // that differ in the middle of the block.
    const auto first_value = points.packed_value(leaf.from).packed_value();
    std::vector<uint8_t> current(first_value.begin(), first_value.end());
    info.common_prefix_lengths.assign(num_dims, bytes_per_dim);
    for (size_t i = 1; i < count; ++i) {
        const auto value = points.packed_value(leaf.from + i).packed_value();
        for (size_t dim = 0; dim < num_dims; ++dim) {
            if (info.common_prefix_lengths[dim] == 0) {
                continue;
            }
            const size_t offset = dim * bytes_per_dim;
            const size_t prefix = static_cast<size_t>(
                BKDUtil::common_prefix_length_n(current, offset, value, offset, bytes_per_dim));
            info.common_prefix_lengths[dim] = std::min(info.common_prefix_lengths[dim], prefix);
        }
    }

    size_t prefix_sum = 0;
    for (size_t dim = 0; dim < num_dims; ++dim) {
        const size_t prefix = info.common_prefix_lengths[dim];
        prefix_sum += prefix;
        if (auto written = out.write_vint(static_cast<int32_t>(prefix)); !written.has_value()) {
            return err(written.error());
        }
        if (prefix > 0) {
            if (auto written = out.write_bytes(std::span<const uint8_t>(current.data() + dim * bytes_per_dim, prefix));
                !written.has_value()) {
                return err(written.error());
            }
        }
    }

    // ---- packed values ----
    //
    // Three forms are possible (rlucene's `write_leaf_block_packed_values`):
    //   * `0xFF`              every value equal;
    //   * `0xFE`              one `(vint runLength, suffix)` pair per distinct value;
    //   * `sorted_dim` (< numDims)  run-length over the byte at
    //     `sorted_dim * bytesPerDim + commonPrefix[sorted_dim]`, other suffix
    //     bytes stored verbatim per point.
    // The two variable-length forms are costed before writing because their sizes
    // differ by more than a constant.
    const size_t packed_length = config.packed_bytes_length();
    if (prefix_sum == packed_length) {
        // Every dimension is constant across the block: one byte says it all.
        info.cardinality = 1;
        if (auto written = out.write_byte(kAllEqual); !written.has_value()) {
            return err(written.error());
        }
        return info;
    }

    const size_t sorted_dim = pick_sorted_dim(leaf.sorted_dim, info.common_prefix_lengths, num_dims, bytes_per_dim);
    const size_t compressed_byte_offset = sorted_dim * bytes_per_dim + info.common_prefix_lengths[sorted_dim];
    const size_t cardinality = points.compute_cardinality(leaf.from, leaf.to, info.common_prefix_lengths);
    info.cardinality = cardinality;
    info.sorted_dim = sorted_dim;

    // Cost of the run-length form: one prefix byte plus one length byte per run,
    // and one byte less per stored suffix.
    size_t num_runs = 0;
    for (size_t i = 0; i < count;) {
        const uint8_t bucket = points.packed_value(leaf.from + i).packed_value()[compressed_byte_offset];
        ++num_runs;
        size_t run = 1;
        while (i + run < count && run < 0xFF &&
               points.packed_value(leaf.from + i + run).packed_value()[compressed_byte_offset] == bucket) {
            ++run;
        }
        i += run;
    }

    const size_t suffix_total = packed_length - prefix_sum;
    size_t high_cardinality_cost = 0;
    size_t low_cardinality_cost = 0;
    if (cardinality == count) {
        // All values differ: the run-length form degenerates to two bytes per run.
        high_cardinality_cost = 0;
        low_cardinality_cost = 1;
    } else {
        high_cardinality_cost = count * (suffix_total - 1) + 2 * num_runs;
        low_cardinality_cost = cardinality * (suffix_total + 1);
    }

    if (low_cardinality_cost <= high_cardinality_cost) {
        if (auto written = out.write_byte(kLowCardinality); !written.has_value()) {
            return err(written.error());
        }
        if (config.num_index_dims != 1) {
            if (auto written = write_actual_bounds(points, leaf, info.common_prefix_lengths, config.num_index_dims,
                                                   bytes_per_dim, out);
                !written.has_value()) {
                return err(written.error());
            }
        }

        // `current` holds the last distinct value seen, so a pair can be flushed
        // the moment the value changes; the final pair is flushed after the loop.
        size_t run_length = 1;
        for (size_t i = 1; i < count; ++i) {
            const auto value = points.packed_value(leaf.from + i).packed_value();
            if (suffix_equals(value, current, info.common_prefix_lengths, num_dims, bytes_per_dim)) {
                ++run_length;
                continue;
            }

            if (auto written = out.write_vint(static_cast<int32_t>(run_length)); !written.has_value()) {
                return err(written.error());
            }
            if (auto written = write_suffixes(out, current, info.common_prefix_lengths, num_dims, bytes_per_dim);
                !written.has_value()) {
                return err(written.error());
            }

            std::copy(value.begin(), value.end(), current.begin());
            run_length = 1;
        }

        if (auto written = out.write_vint(static_cast<int32_t>(run_length)); !written.has_value()) {
            return err(written.error());
        }
        if (auto written = write_suffixes(out, current, info.common_prefix_lengths, num_dims, bytes_per_dim);
            !written.has_value()) {
            return err(written.error());
        }
        return info;
    }

    // Run-length form: the marker byte *is* the compressed dimension.
    if (auto written = out.write_byte(static_cast<uint8_t>(sorted_dim)); !written.has_value()) {
        return err(written.error());
    }
    if (config.num_index_dims != 1) {
        if (auto written = write_actual_bounds(points, leaf, info.common_prefix_lengths, config.num_index_dims,
                                               bytes_per_dim, out);
            !written.has_value()) {
            return err(written.error());
        }
    }

    // The compressed byte is stored once per run, so it is no longer part of the
    // per-point suffix.
    auto suffixes = info.common_prefix_lengths;
    ++suffixes[sorted_dim];

    for (size_t i = 0; i < count;) {
        const auto first_value = points.packed_value(leaf.from + i).packed_value();
        const uint8_t bucket = first_value[compressed_byte_offset];
        size_t run = 1;
        while (i + run < count && run < 0xFF &&
               points.packed_value(leaf.from + i + run).packed_value()[compressed_byte_offset] == bucket) {
            ++run;
        }

        if (auto written = out.write_byte(bucket); !written.has_value()) {
            return err(written.error());
        }
        if (auto written = out.write_byte(static_cast<uint8_t>(run)); !written.has_value()) {
            return err(written.error());
        }
        for (size_t j = 0; j < run; ++j) {
            const auto value = points.packed_value(leaf.from + i + j).packed_value();
            if (auto written = write_suffixes(out, value, suffixes, num_dims, bytes_per_dim); !written.has_value()) {
                return err(written.error());
            }
        }
        i += run;
    }
    return info;
}

} // namespace lucene
