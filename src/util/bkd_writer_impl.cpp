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

module lucene_core:bkd_writer.impl;

import :bkd_writer;

import :bkd_config;
import :bkd_heap_point_writer;
import :bkd_leaf_writer;
import :bkd_offline_point;
import :bkd_point_writer;
import :bkd_radix_selector;
import :bkd_tree_planner;
import :bkd_util;
import :bit_util;
import :codec_util;
import :data_output;
import :directory;
import :error;
import :growable_byte_array_data_output;
import :index_output;
import :types;

import std;

namespace lucene {

namespace {

/// Prefix-codes the split values of a [`BkdPlan`] into the packed index.
///
/// Port of `pack_index`/`recurse_pack_index` in rlucene.  The byte layout of one
/// inner node is:
///
/// ```text
/// [vlong delta of the left subtree's first leaf block]   // only when the node
///                                                        // is not the leftmost
/// vint code          // (byteDelta * (1 + bytesPerDim) + prefix) * numIndexDims + splitDim
/// suffix bytes       // split value past the prefix and past the changed byte
/// vint leftNumBytes  // omitted when the left child is a leaf
/// <left subtree>
/// <right subtree>
/// ```
///
/// `lastSplitValues` is shared state carried down the recursion so each node only
/// stores the bytes that differ from the split value last seen in that dimension;
/// `negativeDeltas` records that a dimension is descending inside the left
/// subtree, which flips the sign of the changed-byte delta.
///
/// **Deviation:** rlucene buffers the left subtree in a block list to learn its
/// length before writing `leftNumBytes`.  Returning each subtree as a byte vector
/// is the same layout with simpler bookkeeping, at the cost of O(size * depth)
/// copying.
class IndexPacker {
public:
    IndexPacker(const BKDConfig &config, const BkdPlan &plan, std::span<const size_t> leaf_block_fps)
        : config_(config), plan_(plan), leaf_block_fps_(leaf_block_fps),
          last_split_values_(config.num_index_dims * config.bytes_per_dim, 0),
          negative_deltas_(config.num_index_dims, false) {}

    [[nodiscard]] Result<std::vector<uint8_t>> pack() {
        return pack_subtree(0, plan_.leaves.size(), /*is_left=*/false, /*min_block_fp=*/0);
    }

private:
    [[nodiscard]] Result<std::vector<uint8_t>> pack_subtree(size_t leaves_offset, size_t num_leaves, bool is_left,
                                                           size_t min_block_fp) {
        GrowableByteArrayDataOutput out;

        if (num_leaves == 1) {
            if (!is_left) {
                // The caller reads this as "the first block of this subtree starts
                // `delta` bytes after the previous one".
                const size_t delta = leaf_block_fps_[leaves_offset] - min_block_fp;
                if (auto written = out.write_vlong(static_cast<int64_t>(delta)); !written.has_value()) {
                    return err(written.error());
                }
            }
            return out.take_bytes();
        }

        size_t left_block_fp = min_block_fp;
        if (!is_left) {
            left_block_fp = leaf_block_fps_[leaves_offset];
            const size_t delta = left_block_fp - min_block_fp;
            if (auto written = out.write_vlong(static_cast<int64_t>(delta)); !written.has_value()) {
                return err(written.error());
            }
        }

        const size_t num_left = BkdTreePlanner::num_left_leaf_nodes(num_leaves);
        const size_t right_offset = leaves_offset + num_left;
        const BkdSplit &split = plan_.splits[right_offset - 1];
        const size_t bytes_per_dim = config_.bytes_per_dim;
        const size_t base = split.dim * bytes_per_dim;

        const size_t prefix = static_cast<size_t>(
            BKDUtil::common_prefix_length_n(split.value, 0, last_split_values_, base, bytes_per_dim));

        size_t first_diff_byte_delta = 0;
        if (prefix < bytes_per_dim) {
            const int32_t diff = static_cast<int32_t>(split.value[prefix]) -
                                 static_cast<int32_t>(last_split_values_[base + prefix]);
            // The reader rebuilds this byte by adding a signed delta whose sign
            // comes from `negative_deltas`, so the encoded magnitude has to fit a
            // byte in that direction.  Zero is legal — it happens when a node's
            // split value equals the one inherited for that dimension.
            const int32_t delta = negative_deltas_[split.dim] ? -diff : diff;
            if (delta < 0 || delta > 255) {
                return err(LuceneError::illegal_state("the split value delta does not fit the packed index"));
            }
            first_diff_byte_delta = static_cast<size_t>(delta);
        }

        const int64_t code = (static_cast<int64_t>(first_diff_byte_delta) * (1 + static_cast<int64_t>(bytes_per_dim)) +
                              static_cast<int64_t>(prefix)) *
                                 static_cast<int64_t>(config_.num_index_dims) +
                             static_cast<int64_t>(split.dim);
        if (code > std::numeric_limits<int32_t>::max()) {
            return err(LuceneError::illegal_state("the packed index node code does not fit into an int32"));
        }
        if (auto written = out.write_vint(static_cast<int32_t>(code)); !written.has_value()) {
            return err(written.error());
        }

        const size_t suffix = bytes_per_dim - prefix;
        if (suffix > 1) {
            // The changed byte is carried by `code`, so only the bytes after it
            // are stored.
            const auto changed = std::span<const uint8_t>(split.value.data() + prefix + 1, suffix - 1);
            if (auto written = out.write_bytes(changed, 0, changed.size()); !written.has_value()) {
                return err(written.error());
            }
        }

        // `saved` is the value inherited from the ancestors (what the parent's
        // sibling needs back), `own` is this node's value.  rlucene keeps a
        // per-level stack of split values; with a single array the two snapshots
        // reproduce the same thing: the left subtree is encoded against `own`
        // (negative delta), the right subtree against `own` too (positive delta),
        // and the ancestors' value comes back once this subtree is done.
        std::vector<uint8_t> saved(last_split_values_.begin() + static_cast<std::ptrdiff_t>(base + prefix),
                                   last_split_values_.begin() + static_cast<std::ptrdiff_t>(base + prefix + suffix));
        const std::vector<uint8_t> own(split.value.begin() + static_cast<std::ptrdiff_t>(prefix),
                                       split.value.begin() + static_cast<std::ptrdiff_t>(prefix + suffix));
        std::copy(own.begin(), own.end(), last_split_values_.begin() + static_cast<std::ptrdiff_t>(base + prefix));

        const bool saved_negative = negative_deltas_[split.dim];
        negative_deltas_[split.dim] = true;

        auto left = pack_subtree(leaves_offset, num_left, /*is_left=*/true, left_block_fp);
        if (!left.has_value()) {
            return err(left.error());
        }
        // The left subtree length sits *before* the subtree it describes: the
        // reader needs it to know where the right subtree starts.
        if (num_left != 1) {
            if (left->size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
                return err(LuceneError::illegal_state("the left subtree index does not fit into an int32"));
            }
            if (auto written = out.write_vint(static_cast<int32_t>(left->size())); !written.has_value()) {
                return err(written.error());
            }
        }
        if (auto written = out.write_bytes(left.value(), 0, left->size()); !written.has_value()) {
            return err(written.error());
        }

        negative_deltas_[split.dim] = false;

        // The right subtree's deltas are relative to *this* node's split value,
        // not to whichever value the left subtree ended on.
        std::copy(own.begin(), own.end(), last_split_values_.begin() + static_cast<std::ptrdiff_t>(base + prefix));

        auto right = pack_subtree(right_offset, num_leaves - num_left, /*is_left=*/false, left_block_fp);
        if (!right.has_value()) {
            return err(right.error());
        }
        if (auto written = out.write_bytes(right.value(), 0, right->size()); !written.has_value()) {
            return err(written.error());
        }

        negative_deltas_[split.dim] = saved_negative;
        std::copy(saved.begin(), saved.end(),
                  last_split_values_.begin() + static_cast<std::ptrdiff_t>(base + prefix));
        return out.take_bytes();
    }

    const BKDConfig &config_;
    const BkdPlan &plan_;
    std::span<const size_t> leaf_block_fps_;
    std::vector<uint8_t> last_split_values_;
    std::vector<uint8_t> negative_deltas_;
};

// ---- spilled (external) build ----

/// A half-open range of points inside a point writer.
struct PointSlice {
    PointWriterVariant writer{static_cast<HeapPointWriter *>(nullptr)};
    size_t start{0};
    size_t count{0};
};

/// Visits every point of `slice` in write order.
///
/// Heap slices are indexed directly; spilled slices are streamed through an
/// [`OfflinePointReader`] with a small reusable buffer, which is what keeps the
/// external build's working set bounded.
template <typename Visitor>
[[nodiscard]] VoidResult for_each_point(const BKDConfig &config, const Directory &dir, const PointSlice &slice,
                                        std::vector<uint8_t> &buffer, Visitor &&visitor) {
    if (auto *heap = PointWriters::heap_writer(slice.writer); heap != nullptr) {
        for (size_t i = slice.start; i < slice.start + slice.count; ++i) {
            auto &value = heap->packed_value(i);
            if (auto visited = visitor(value.packed_value(), value.doc_id()); !visited.has_value()) {
                return visited;
            }
        }
        return {};
    }
    const auto *offline = PointWriters::offline_writer(slice.writer);
    if (offline == nullptr) {
        return err(LuceneError::illegal_state("the point slice has no writer"));
    }
    auto reader = offline->make_reader(dir, slice.start, slice.count, buffer);
    if (!reader.has_value()) {
        return err(reader.error());
    }
    for (size_t i = 0; i < slice.count; ++i) {
        auto has_next = reader->next();
        if (!has_next.has_value()) {
            return err(has_next.error());
        }
        if (!has_next.value()) {
            return err(LuceneError::illegal_state("the spilled point slice ended before its announced count"));
        }
        auto &value = reader->point_value();
        if (auto visited = visitor(value.packed_value(), value.doc_id()); !visited.has_value()) {
            return visited;
        }
    }
    return reader->close();
}

/// Builds leaves out of a spilled point set, one bounded heap block at a time.
///
/// Port of the `build`/`select` half of rlucene's `BKDWriter` for the offline
/// case: a slice is split by an MSD radix pass over one dimension until the
/// boundary between the two children is a real byte boundary, and every leaf is
/// then materialised into a heap writer of at most `max_points_in_leaf` points
/// so the regular leaf encoder can be reused unchanged.
class ExternalBuilder {
public:
    ExternalBuilder(const BKDConfig &config, Directory &dir, size_t max_points_in_leaf)
        : config_(config), dir_(&dir), max_points_in_leaf_(max_points_in_leaf),
          buffer_(config.bytes_per_doc() * 64, 0) {}

    /// Writes the leaves of a subtree with `num_leaves` leaves covering `slice`,
    /// in write order.
    [[nodiscard]] VoidResult build(size_t leaves_offset, size_t num_leaves, PointSlice slice,
                                   std::optional<size_t> sort_dim, IndexOutput &out, std::vector<size_t> &fps,
                                   std::vector<BkdSplit> &splits);

    [[nodiscard]] VoidResult cleanup();

private:
    /// Splits `slice` so that exactly `left_count` points stay on the left.
    [[nodiscard]] Result<std::vector<uint8_t>> partition(const PointSlice &slice, size_t left_count, size_t split_dim,
                                                         PointSlice &left, PointSlice &right,
                                                         std::unique_ptr<OfflinePointWriter> &left_writer,
                                                         std::unique_ptr<OfflinePointWriter> &right_writer);

    /// Index dimension with the widest value range, or `0` when all of them are
    /// constant (mirrors [`BkdTreePlanner::choose_split_dim`]).
    [[nodiscard]] Result<size_t> choose_split_dim(const PointSlice &slice);

    const BKDConfig &config_;
    Directory *dir_;
    size_t max_points_in_leaf_{0};
    std::vector<uint8_t> buffer_;
    /// Temporary files created by `partition` and by the leaf materialisation,
    /// deleted in [`cleanup`] no matter how the build ends.
    std::vector<std::string> temp_files_;
};

VoidResult ExternalBuilder::cleanup() {
    for (const auto &name : temp_files_) {
        (void)dir_->delete_file(name);
    }
    temp_files_.clear();
    return {};
}

Result<size_t> ExternalBuilder::choose_split_dim(const PointSlice &slice) {
    const size_t bytes_per_dim = config_.bytes_per_dim;
    const size_t total = config_.num_index_dims * bytes_per_dim;
    std::vector<uint8_t> min_value(total, 0xFF);
    std::vector<uint8_t> max_value(total, 0x00);
    bool first = true;
    auto scanned = for_each_point(config_, *dir_, slice, buffer_,
                                  [&](std::span<const uint8_t> value, int32_t) -> VoidResult {
                                      if (first) {
                                          std::copy_n(value.begin(), total, min_value.begin());
                                          std::copy_n(value.begin(), total, max_value.begin());
                                          first = false;
                                          return {};
                                      }
                                      for (size_t dim = 0; dim < config_.num_index_dims; ++dim) {
                                          const size_t offset = dim * bytes_per_dim;
                                          if (BKDUtil::compare_unsigned(value, offset, min_value, offset, bytes_per_dim) < 0) {
                                              std::copy_n(value.begin() + static_cast<std::ptrdiff_t>(offset),
                                                          bytes_per_dim,
                                                          min_value.begin() + static_cast<std::ptrdiff_t>(offset));
                                          } else if (BKDUtil::compare_unsigned(value, offset, max_value, offset,
                                                                              bytes_per_dim) > 0) {
                                              std::copy_n(value.begin() + static_cast<std::ptrdiff_t>(offset),
                                                          bytes_per_dim,
                                                          max_value.begin() + static_cast<std::ptrdiff_t>(offset));
                                          }
                                      }
                                      return {};
                                  });
    if (!scanned.has_value()) {
        return err(scanned.error());
    }
    if (first) {
        return err(LuceneError::illegal_argument("cannot split an empty point slice"));
    }

    // Same ranking as the planner: the widest numeric range wins, and a fully
    // constant block falls back to dimension 0 (the packer accepts a degenerate
    // split, which is what keeps the quota shape authoritative).
    size_t best_dim = 0;
    size_t best_range = 0;
    bool any_range = false;
    for (size_t dim = 0; dim < config_.num_index_dims; ++dim) {
        const size_t offset = dim * bytes_per_dim;
        size_t range = 0;
        bool varies = false;
        for (size_t b = 0; b < bytes_per_dim; ++b) {
            if (min_value[offset + b] != max_value[offset + b]) {
                varies = true;
            }
            const size_t contribution = static_cast<size_t>(max_value[offset + b] - min_value[offset + b])
                                        << (8 * (bytes_per_dim - 1 - b));
            range = std::max(range, contribution);
        }
        if (!varies) {
            continue;
        }
        if (!any_range || range > best_range) {
            any_range = true;
            best_range = range;
            best_dim = dim;
        }
    }
    return best_dim;
}

Result<std::vector<uint8_t>> ExternalBuilder::partition(const PointSlice &slice, size_t left_count, size_t split_dim,
                                                        PointSlice &left, PointSlice &right,
                                                        std::unique_ptr<OfflinePointWriter> &left_writer,
                                                        std::unique_ptr<OfflinePointWriter> &right_writer) {
    const size_t bytes_per_dim = config_.bytes_per_dim;
    const size_t dim_offset = split_dim * bytes_per_dim;

    auto created_left = OfflinePointWriter::create(config_, *dir_, "bkd", "left", 0);
    if (!created_left.has_value()) {
        return err(created_left.error());
    }
    left_writer = std::make_unique<OfflinePointWriter>(std::move(created_left.value()));
    temp_files_.push_back(left_writer->name());

    auto created_right = OfflinePointWriter::create(config_, *dir_, "bkd", "right", 0);
    if (!created_right.has_value()) {
        return err(created_right.error());
    }
    right_writer = std::make_unique<OfflinePointWriter>(std::move(created_right.value()));
    temp_files_.push_back(right_writer->name());

    // `target` is the index, inside the current group, at which the left side
    // must end.  `level` is the byte of `split_dim` the current pass looks at.
    size_t target = left_count;
    size_t level = 0;
    PointSlice current = slice;
    std::vector<uint8_t> split_value(bytes_per_dim, 0);
    /// Points of the bucket the descent is currently narrowing down.
    std::unique_ptr<OfflinePointWriter> middle;
    /// Same file, kept alive as long as `current` points into it.
    std::unique_ptr<OfflinePointWriter> current_owner;

    while (true) {
        const bool positional = level >= bytes_per_dim;
        size_t bucket = 0;
        size_t bucket_start = 0;
        bool aligned = false;
        if (!positional) {
            const size_t byte = dim_offset + level;
            std::array<size_t, 257> cumulative{};
            auto counted = for_each_point(config_, *dir_, current, buffer_,
                                          [&](std::span<const uint8_t> value, int32_t) -> VoidResult {
                                              ++cumulative[value[byte] + 1];
                                              return {};
                                          });
            if (!counted.has_value()) {
                return err(counted.error());
            }
            for (size_t b = 0; b < 256; ++b) {
                cumulative[b + 1] += cumulative[b];
            }
            debug_check(cumulative[256] == current.count, "the histogram must cover the whole group");
            while (cumulative[bucket + 1] <= target) {
                ++bucket;
            }
            bucket_start = cumulative[bucket];
            // The split is clean as soon as `target` lands on the first point of
            // a bucket: nothing has to be routed into a scratch file.
            aligned = bucket_start == target;

            if (!aligned) {
                auto created_middle = OfflinePointWriter::create(config_, *dir_, "bkd", "middle", 0);
                if (!created_middle.has_value()) {
                    return err(created_middle.error());
                }
                middle = std::make_unique<OfflinePointWriter>(std::move(created_middle.value()));
                temp_files_.push_back(middle->name());
            }
        }

        // Route the group: below the bucket goes left, above goes right, and the
        // bucket itself either goes right (clean boundary) or becomes the next
        // level's group.
        size_t index = 0;
        bool captured = false;
        auto routed = for_each_point(config_, *dir_, current, buffer_,
                                     [&](std::span<const uint8_t> value, int32_t doc_id) -> VoidResult {
                                         if (index == target) {
                                             // The bytes before this level are shared by the
                                             // whole group; the byte at this level is the bucket
                                             // the right side starts at (the point sitting on the
                                             // boundary is just some member of that bucket, so its
                                             // own low bytes say nothing).  A positional split,
                                             // where every byte of the dimension is equal, keeps
                                             // the point's value.
                                             std::copy_n(value.begin() + static_cast<std::ptrdiff_t>(dim_offset),
                                                         bytes_per_dim, split_value.begin());
                                             if (!positional && aligned) {
                                                 split_value[level] = static_cast<uint8_t>(bucket);
                                             }
                                             captured = true;
                                         }
                                         ++index;
                                         if (positional) {
                                             const bool to_left = index <= target;
                                             return PointWriters::append_bytes(
                                                 PointWriterVariant{to_left ? left_writer.get() : right_writer.get()},
                                                 value, doc_id);
                                         }
                                         const uint8_t byte = value[dim_offset + level];
                                         if (byte < bucket) {
                                             return PointWriters::append_bytes(
                                                 PointWriterVariant{left_writer.get()}, value, doc_id);
                                         }
                                         if (byte > bucket || aligned) {
                                             return PointWriters::append_bytes(
                                                 PointWriterVariant{right_writer.get()}, value, doc_id);
                                         }
                                         return PointWriters::append_bytes(PointWriterVariant{middle.get()}, value,
                                                                           doc_id);
                                     });
        if (!routed.has_value()) {
            return err(routed.error());
        }
        if (!captured) {
            return err(LuceneError::illegal_state("the partition point was never reached"));
        }

        if (positional || aligned) {
            break;
        }

        // Descend: everything before the bucket is already on the left, so the
        // next level only has to place `target - bucket_start` of its points.
        if (auto closed = middle->close(); !closed.has_value()) {
            return err(closed.error());
        }
        target -= bucket_start;
        ++level;
        current_owner = std::move(middle);
        current = PointSlice{PointWriterVariant{current_owner.get()}, 0, current_owner->count()};
    }

    // Zero the bytes the descent never compared: the split value has to be the
    // smallest value on the right side, not whatever low bytes the point that
    // happens to sit on the boundary carries.  A positional split (every byte of
    // the dimension is equal inside the group) keeps the group's value as is.
    if (level < bytes_per_dim) {
        std::fill(split_value.begin() + static_cast<std::ptrdiff_t>(level + 1), split_value.end(), 0);
    }

    if (auto closed = left_writer->close(); !closed.has_value()) {
        return err(closed.error());
    }
    if (auto closed = right_writer->close(); !closed.has_value()) {
        return err(closed.error());
    }
    left = PointSlice{PointWriterVariant{left_writer.get()}, 0, left_writer->count()};
    right = PointSlice{PointWriterVariant{right_writer.get()}, 0, right_writer->count()};
    debug_check(left.count == left_count, "the left side must hold exactly the requested count");
    return split_value;
}

VoidResult ExternalBuilder::build(size_t leaves_offset, size_t num_leaves, PointSlice slice,
                                  std::optional<size_t> sort_dim, IndexOutput &out, std::vector<size_t> &fps,
                                  std::vector<BkdSplit> &splits) {
    if (num_leaves == 1) {
        debug_check(slice.count > 0 && slice.count <= max_points_in_leaf_, "a leaf must fit the leaf quota");

        // Bounded materialisation: at most `max_points_in_leaf` points in memory.
        HeapPointWriter heap(config_, slice.count);
        auto copied = for_each_point(config_, *dir_, slice, buffer_,
                                     [&](std::span<const uint8_t> value, int32_t doc_id) -> VoidResult {
                                         return heap.append_bytes(value, doc_id);
                                     });
        if (!copied.has_value()) {
            return copied;
        }
        if (auto closed = heap.close(); !closed.has_value()) {
            return closed;
        }
        const size_t sorted = sort_dim.value_or(0);
        if (sort_dim.has_value() && slice.count > 1) {
            BkdRadixSelector selector(heap);
            if (auto sorted_ok = selector.sort(0, slice.count, sorted * config_.bytes_per_dim); !sorted_ok.has_value()) {
                return sorted_ok;
            }
        }
        auto position = out.get_file_pointer();
        if (!position.has_value()) {
            return err(position.error());
        }
        fps[leaves_offset] = position.value();
        auto written = BkdLeafWriter::write_leaf_block(heap, BkdLeaf{0, slice.count, sorted}, out);
        if (!written.has_value()) {
            return err(written.error());
        }
        return {};
    }

    const size_t num_left = BkdTreePlanner::num_left_leaf_nodes(num_leaves);
    const size_t left_count = num_left * max_points_in_leaf_;
    if (left_count == 0 || left_count >= slice.count) {
        return err(LuceneError::illegal_state("the leaf quota does not fit the point range"));
    }

    const size_t split_dim = config_.num_index_dims == 1 ? 0 : choose_split_dim(slice).value_or(0);

    PointSlice left{};
    PointSlice right{};
    std::unique_ptr<OfflinePointWriter> left_writer;
    std::unique_ptr<OfflinePointWriter> right_writer;
    auto split_value = partition(slice, left_count, split_dim, left, right, left_writer, right_writer);
    if (!split_value.has_value()) {
        return err(split_value.error());
    }
    splits[leaves_offset + num_left - 1] = BkdSplit{split_dim, std::move(split_value.value())};

    if (auto built = build(leaves_offset, num_left, left, split_dim, out, fps, splits); !built.has_value()) {
        return built;
    }
    return build(leaves_offset + num_left, num_leaves - num_left, right, split_dim, out, fps, splits);
}

} // namespace

BkdWriter::BkdWriter(const BKDConfig &config, size_t total_point_count, int32_t max_doc, Directory *dir,
                     size_t max_points_in_heap)
    : config_(config), dir_(dir), max_points_in_heap_(max_points_in_heap), total_point_count_(total_point_count),
      max_doc_(max_doc), docs_seen_(static_cast<size_t>(max_doc), 0),
      min_packed_value_(config.packed_index_bytes_length(), 0),
      max_packed_value_(config.packed_index_bytes_length(), 0) {
    // The spill decision is made here, once: it only depends on the announced
    // point count and the heap budget.
    if (dir != nullptr && max_points_in_heap > 0 && total_point_count > max_points_in_heap) {
        auto offline = OfflinePointWriter::create(config_, *dir_, "bkd", "spill", 0);
        if (!offline.has_value()) {
            // Fall back to memory rather than failing construction; `finish`
            // reports the allocation failure if the block does not fit either.
            heap_writer_ = std::make_unique<HeapPointWriter>(config_, total_point_count);
            max_points_in_heap_ = 0;
            return;
        }
        offline_writer_ = std::make_unique<OfflinePointWriter>(std::move(offline.value()));
        return;
    }
    heap_writer_ = std::make_unique<HeapPointWriter>(config_, total_point_count);
}

BkdWriter::~BkdWriter() { (void)close(); }

BkdWriter::BkdWriter(BkdWriter &&) noexcept = default;
BkdWriter &BkdWriter::operator=(BkdWriter &&) noexcept = default;

Result<BkdWriter> BkdWriter::create(const BKDConfig &config, size_t total_point_count, int32_t max_doc) {
    return create(config, total_point_count, max_doc, nullptr, 0);
}

Result<BkdWriter> BkdWriter::create(const BKDConfig &config, size_t total_point_count, int32_t max_doc,
                                    Directory *dir, size_t max_points_in_heap) {
    if (total_point_count == 0) {
        return err(LuceneError::illegal_argument("a BKD writer needs room for at least one point"));
    }
    if (max_doc <= 0) {
        return err(LuceneError::illegal_argument("max_doc must be positive"));
    }
    return BkdWriter(config, total_point_count, max_doc, dir, max_points_in_heap);
}

VoidResult BkdWriter::add(std::span<const uint8_t> packed_value, int32_t doc_id) {
    if (closed_) {
        return err(LuceneError::illegal_state("the BKD writer is closed"));
    }
    if (finished_) {
        return err(LuceneError::illegal_state("the BKD writer is already finished"));
    }
    if (packed_value.size() != config_.packed_bytes_length()) {
        return err(LuceneError::illegal_argument("a point must be packed_bytes_length() bytes long"));
    }
    if (doc_id < 0 || doc_id >= max_doc_) {
        return err(LuceneError::illegal_argument("the doc id is outside [0, max_doc)"));
    }
    if (point_count_ >= total_point_count_) {
        return err(LuceneError::illegal_state("more points were added than announced in total_point_count"));
    }

    VoidResult appended = heap_writer_ != nullptr
                              ? heap_writer_->append_bytes(packed_value, doc_id)
                              : offline_writer_->append_bytes(packed_value, doc_id);
    if (!appended.has_value()) {
        return appended;
    }
    ++point_count_;

    if (docs_seen_[static_cast<size_t>(doc_id)] == 0) {
        docs_seen_[static_cast<size_t>(doc_id)] = 1;
        ++doc_count_;
    }

    // Bounds of each indexed dimension, prefix-coding in the packed index relies
    // on knowing the extremes of the whole tree.
    const size_t bytes_per_dim = config_.bytes_per_dim;
    for (size_t dim = 0; dim < config_.num_index_dims; ++dim) {
        const size_t offset = dim * bytes_per_dim;
        if (point_count_ == 1) {
            std::copy_n(packed_value.begin() + static_cast<std::ptrdiff_t>(offset), bytes_per_dim,
                        min_packed_value_.begin() + static_cast<std::ptrdiff_t>(offset));
            std::copy_n(packed_value.begin() + static_cast<std::ptrdiff_t>(offset), bytes_per_dim,
                        max_packed_value_.begin() + static_cast<std::ptrdiff_t>(offset));
            continue;
        }
        if (BKDUtil::compare_unsigned(packed_value, offset, min_packed_value_, offset, bytes_per_dim) < 0) {
            std::copy_n(packed_value.begin() + static_cast<std::ptrdiff_t>(offset), bytes_per_dim,
                        min_packed_value_.begin() + static_cast<std::ptrdiff_t>(offset));
        } else if (BKDUtil::compare_unsigned(packed_value, offset, max_packed_value_, offset, bytes_per_dim) > 0) {
            std::copy_n(packed_value.begin() + static_cast<std::ptrdiff_t>(offset), bytes_per_dim,
                        max_packed_value_.begin() + static_cast<std::ptrdiff_t>(offset));
        }
    }
    return {};
}

Result<BkdWriteResult> BkdWriter::finish_on_heap(IndexOutput &data_out) {
    BkdWriteResult result;
    result.point_count = point_count_;
    result.doc_count = doc_count_;
    result.count_per_leaf = config_.max_points_in_leaf_node;

    if (auto closed = heap_writer_->close(); !closed.has_value()) {
        return err(closed.error());
    }

    auto start_fp = data_out.get_file_pointer();
    if (!start_fp.has_value()) {
        return err(start_fp.error());
    }
    result.data_start_fp = start_fp.value();

    BkdTreePlanner planner(*heap_writer_, config_.max_points_in_leaf_node);
    auto plan = planner.plan();
    if (!plan.has_value()) {
        return err(plan.error());
    }

    result.leaf_block_fps.reserve(plan->leaves.size());
    for (const auto &leaf : plan->leaves) {
        auto leaf_fp = data_out.get_file_pointer();
        if (!leaf_fp.has_value()) {
            return err(leaf_fp.error());
        }
        result.leaf_block_fps.push_back(leaf_fp.value());

        auto written = BkdLeafWriter::write_leaf_block(*heap_writer_, leaf, data_out);
        if (!written.has_value()) {
            return err(written.error());
        }
    }

    IndexPacker packer(config_, *plan, result.leaf_block_fps);
    auto packed = packer.pack();
    if (!packed.has_value()) {
        return err(packed.error());
    }
    result.packed_index = std::move(packed.value());
    return result;
}

Result<BkdWriteResult> BkdWriter::finish(IndexOutput &data_out) {
    if (finished_) {
        return err(LuceneError::illegal_state("the BKD writer is already finished"));
    }
    finished_ = true;

    BkdWriteResult result;
    result.point_count = point_count_;
    result.doc_count = doc_count_;
    result.count_per_leaf = config_.max_points_in_leaf_node;

    if (point_count_ == 0) {
        // rlucene returns `None` here; an empty result carries the same meaning.
        return result;
    }

    if (heap_writer_ != nullptr) {
        return finish_on_heap(data_out);
    }

    // ---- spilled build ----
    if (auto closed = offline_writer_->close(); !closed.has_value()) {
        return err(closed.error());
    }
    auto start_fp = data_out.get_file_pointer();
    if (!start_fp.has_value()) {
        return err(start_fp.error());
    }
    result.data_start_fp = start_fp.value();

    const size_t max_leaf = config_.max_points_in_leaf_node;
    const size_t num_leaves = (point_count_ + max_leaf - 1) / max_leaf;
    result.leaf_block_fps.assign(num_leaves, 0);
    std::vector<BkdSplit> splits(num_leaves > 1 ? num_leaves - 1 : 0);

    ExternalBuilder builder(config_, *dir_, max_leaf);
    PointSlice whole{PointWriterVariant{offline_writer_.get()}, 0, point_count_};
    auto built = builder.build(0, num_leaves, whole, std::nullopt, data_out, result.leaf_block_fps, splits);
    (void)builder.cleanup();
    if (!built.has_value()) {
        return err(built.error());
    }

    // The packer only needs the leaf count and the splits; the ranges are the
    // heap path's bookkeeping and mean nothing for a spilled build.
    BkdPlan plan;
    plan.leaves.resize(num_leaves);
    plan.splits = std::move(splits);
    IndexPacker packer(config_, plan, result.leaf_block_fps);
    auto packed = packer.pack();
    if (!packed.has_value()) {
        return err(packed.error());
    }
    result.packed_index = std::move(packed.value());
    return result;
}

VoidResult BkdWriter::write_index(IndexOutput &meta_out, IndexOutput *index_out, const BkdWriteResult &data) {
    if (data.packed_index.empty()) {
        return err(LuceneError::illegal_state("no BKD index was produced (no points were added)"));
    }
    if (data.packed_index.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        return err(LuceneError::illegal_state("the packed index does not fit into an int32"));
    }

    if (auto header = CodecUtil::write_header(meta_out, BKD_CODEC_NAME, BKD_VERSION_CURRENT); !header.has_value()) {
        return header;
    }
    if (auto written = meta_out.write_vint(static_cast<int32_t>(config_.num_dims)); !written.has_value()) {
        return written;
    }
    if (auto written = meta_out.write_vint(static_cast<int32_t>(config_.num_index_dims)); !written.has_value()) {
        return written;
    }
    if (auto written = meta_out.write_vint(static_cast<int32_t>(data.count_per_leaf)); !written.has_value()) {
        return written;
    }
    if (auto written = meta_out.write_vint(static_cast<int32_t>(config_.bytes_per_dim)); !written.has_value()) {
        return written;
    }
    if (auto written = meta_out.write_vint(static_cast<int32_t>(data.leaf_block_fps.size())); !written.has_value()) {
        return written;
    }
    if (auto written = meta_out.write_bytes(min_packed_value_); !written.has_value()) {
        return written;
    }
    if (auto written = meta_out.write_bytes(max_packed_value_); !written.has_value()) {
        return written;
    }
    if (auto written = meta_out.write_vlong(static_cast<int64_t>(data.point_count)); !written.has_value()) {
        return written;
    }
    if (auto written = meta_out.write_vint(data.doc_count); !written.has_value()) {
        return written;
    }
    if (auto written = meta_out.write_vint(static_cast<int32_t>(data.packed_index.size())); !written.has_value()) {
        return written;
    }
    if (auto written = meta_out.write_long(static_cast<int64_t>(data.data_start_fp)); !written.has_value()) {
        return written;
    }

    // The index pointer is recorded before this long is written; when both live in
    // the same file the long itself pushes the index 8 bytes further.
    size_t index_fp = 0;
    int64_t same_file_adjust = 0;
    if (index_out == nullptr) {
        auto position = meta_out.get_file_pointer();
        if (!position.has_value()) {
            return err(position.error());
        }
        index_fp = position.value();
        same_file_adjust = static_cast<int64_t>(BitUtil::LONG_BYTES);
    } else {
        auto position = index_out->get_file_pointer();
        if (!position.has_value()) {
            return err(position.error());
        }
        index_fp = position.value();
    }
    if (auto written = meta_out.write_long(static_cast<int64_t>(index_fp) + same_file_adjust); !written.has_value()) {
        return written;
    }

    if (index_out == nullptr) {
        return meta_out.write_bytes(data.packed_index);
    }
    return index_out->write_bytes(data.packed_index);
}

VoidResult BkdWriter::close() {
    if (closed_) {
        return {};
    }
    closed_ = true;
    if (offline_writer_ != nullptr) {
        if (!offline_writer_->closed()) {
            if (auto closed = offline_writer_->close(); !closed.has_value()) {
                return closed;
            }
        }
        if (dir_ != nullptr) {
            (void)offline_writer_->destroy(*dir_);
        }
        offline_writer_.reset();
    }
    heap_writer_.reset();
    return {};
}

} // namespace lucene
