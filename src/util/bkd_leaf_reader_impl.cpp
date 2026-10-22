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

module lucene_core:bkd_leaf_reader.impl;

import :bkd_leaf_reader;

import :bkd_config;
import :bkd_doc_ids_writer;
import :data_input;
import :error;
import :index_input;
import :types;

import std;

namespace lucene {

namespace {

/// Byte that marks "every value in this block is identical".
constexpr uint8_t kAllEqual = 0xFF;
/// Byte that marks the low-cardinality encoding.
constexpr uint8_t kLowCardinality = 0xFE;

/// Glues the stored common prefixes and a suffix into one packed value.
[[nodiscard]] std::vector<uint8_t> assemble_value(const std::vector<std::vector<uint8_t>> &prefixes,
                                                  const std::vector<std::vector<uint8_t>> &suffixes) {
    std::vector<uint8_t> value;
    for (size_t dim = 0; dim < prefixes.size(); ++dim) {
        value.insert(value.end(), prefixes[dim].begin(), prefixes[dim].end());
        value.insert(value.end(), suffixes[dim].begin(), suffixes[dim].end());
    }
    return value;
}

} // namespace

Result<BkdDecodedLeaf> BkdLeafReader::read_leaf_block(IndexInput &data_in, const BKDConfig &config,
                                                      size_t block_fp) {
    if (auto seeked = data_in.seek(block_fp); !seeked.has_value()) {
        return err(seeked.error());
    }

    auto count = data_in.read_vint();
    if (!count.has_value()) {
        return err(count.error());
    }
    if (count.value() <= 0) {
        return err(LuceneError::corrupt_index("a leaf block must hold at least one point"));
    }
    const size_t point_count = static_cast<size_t>(count.value());

    BkdDecodedLeaf leaf;
    leaf.doc_ids.assign(point_count, 0);
    if (auto read = BkdDocIdsWriter::read_ints(data_in, point_count, leaf.doc_ids); !read.has_value()) {
        return err(read.error());
    }

    const size_t bytes_per_dim = config.bytes_per_dim;
    std::vector<std::vector<uint8_t>> prefixes(config.num_dims);
    leaf.common_prefix_lengths.assign(config.num_dims, 0);
    for (size_t dim = 0; dim < config.num_dims; ++dim) {
        auto prefix = data_in.read_vint();
        if (!prefix.has_value()) {
            return err(prefix.error());
        }
        if (prefix.value() < 0 || static_cast<size_t>(prefix.value()) > bytes_per_dim) {
            return err(LuceneError::corrupt_index("the common prefix length is out of range"));
        }
        leaf.common_prefix_lengths[dim] = static_cast<size_t>(prefix.value());
        auto bytes = data_in.get_bytes(static_cast<size_t>(prefix.value()));
        if (!bytes.has_value()) {
            return err(bytes.error());
        }
        prefixes[dim] = std::move(bytes.value());
    }

    auto marker = data_in.read_byte();
    if (!marker.has_value()) {
        return err(marker.error());
    }

    if (marker.value() == kAllEqual) {
        leaf.all_equal = true;
        leaf.cardinality = 1;
        leaf.values.assign(point_count, assemble_value(prefixes, std::vector<std::vector<uint8_t>>(config.num_dims)));
        return leaf;
    }

    // `0xFE` selects the low-cardinality form; any byte below `num_dims` is the
    // high-cardinality form and names the run-length compressed dimension.
    const int8_t marker_as_dim = static_cast<int8_t>(marker.value());
    const bool low_cardinality = marker.value() == kLowCardinality;
    if (!low_cardinality &&
        (marker_as_dim < 0 || static_cast<size_t>(marker_as_dim) >= config.num_dims)) {
        return err(LuceneError::corrupt_index("unexpected BKD leaf encoding " + std::to_string(marker.value())));
    }

    if (config.num_index_dims != 1) {
        // Min/max suffix of every indexed dimension; the leaf encoder writes them
        // for readers that prune without materialising the block.
        for (size_t dim = 0; dim < config.num_index_dims; ++dim) {
            const size_t suffix_length = bytes_per_dim - leaf.common_prefix_lengths[dim];
            if (auto skipped = data_in.skip_bytes(static_cast<int64_t>(2 * suffix_length)); !skipped.has_value()) {
                return err(skipped.error());
            }
        }
    }

    if (!low_cardinality) {
        const size_t compressed_dim = static_cast<size_t>(marker_as_dim);
        if (leaf.common_prefix_lengths[compressed_dim] >= bytes_per_dim) {
            return err(LuceneError::corrupt_index("the compressed dimension has no byte left to compress"));
        }
        leaf.compressed_dim = compressed_dim;

        // The compressed byte is stored once per run, so it drops out of every
        // point's suffix.
        std::vector<size_t> suffixes(leaf.common_prefix_lengths.begin(), leaf.common_prefix_lengths.end());
        ++suffixes[compressed_dim];

        leaf.values.reserve(point_count);
        while (leaf.values.size() < point_count) {
            auto bucket = data_in.read_byte();
            if (!bucket.has_value()) {
                return err(bucket.error());
            }
            auto run_length = data_in.read_byte();
            if (!run_length.has_value()) {
                return err(run_length.error());
            }
            if (run_length.value() == 0) {
                return err(LuceneError::corrupt_index("a leaf run must hold at least one point"));
            }
            if (leaf.values.size() + run_length.value() > point_count) {
                return err(LuceneError::corrupt_index("a leaf run overflows the block"));
            }

            for (size_t j = 0; j < run_length.value(); ++j) {
                std::vector<uint8_t> value;
                for (size_t dim = 0; dim < config.num_dims; ++dim) {
                    value.insert(value.end(), prefixes[dim].begin(), prefixes[dim].end());
                    if (dim == compressed_dim) {
                        value.push_back(bucket.value());
                    }
                    auto suffix = data_in.get_bytes(bytes_per_dim - suffixes[dim]);
                    if (!suffix.has_value()) {
                        return err(suffix.error());
                    }
                    value.insert(value.end(), suffix->begin(), suffix->end());
                }
                leaf.values.push_back(std::move(value));
            }
            ++leaf.cardinality;
        }
        return leaf;
    }

    leaf.values.reserve(point_count);
    while (leaf.values.size() < point_count) {
        auto run_length = data_in.read_vint();
        if (!run_length.has_value()) {
            return err(run_length.error());
        }
        if (run_length.value() <= 0) {
            return err(LuceneError::corrupt_index("a leaf run must hold at least one point"));
        }
        if (leaf.values.size() + static_cast<size_t>(run_length.value()) > point_count) {
            return err(LuceneError::corrupt_index("a leaf run overflows the block"));
        }

        std::vector<std::vector<uint8_t>> suffixes(config.num_dims);
        for (size_t dim = 0; dim < config.num_dims; ++dim) {
            const size_t suffix_length = bytes_per_dim - leaf.common_prefix_lengths[dim];
            auto suffix = data_in.get_bytes(suffix_length);
            if (!suffix.has_value()) {
                return err(suffix.error());
            }
            suffixes[dim] = std::move(suffix.value());
        }

        auto value = assemble_value(prefixes, suffixes);
        ++leaf.cardinality;
        for (int32_t i = 0; i < run_length.value(); ++i) {
            leaf.values.push_back(value);
        }
    }

    return leaf;
}

} // namespace lucene
