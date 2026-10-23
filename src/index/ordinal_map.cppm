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

export module lucene_core:ordinal_map;

import :types;
import std;

import :error;
import :packed_long_values;
import :terms_enum;

namespace lucene {

/// Lists the segments of an [`OrdinalMap`] by descending weight, so the segment
/// that introduces the most terms comes first.
///
/// Port of `SegmentMap` (`core/index/ordinal_map.rs`).
export class SegmentMap {
public:
    /// Orders the segments that have `weights`; ties keep index order.
    [[nodiscard]] static Result<SegmentMap> create(std::span<const int64_t> weights);

    /// The original index of the segment at position `new_index`.
    [[nodiscard]] size_t new_to_old(size_t new_index) const;
    /// The position the segment with original index `old_index` takes.
    [[nodiscard]] size_t old_to_new(size_t old_index) const;
    /// How many segments the map covers.
    [[nodiscard]] size_t size() const noexcept { return new_to_old_.size(); }

private:
    std::vector<int32_t> new_to_old_;
    std::vector<int32_t> old_to_new_;
};

/// Maps the ordinals of one segment to the global ordinal space, as a per-ord
/// delta on top of the segment ordinal.
///
/// Port of the `SegmentToGlobalOrds` values of `core/index/ordinal_map.rs`.
export class SegmentToGlobalOrds {
public:
    /// The identity mapping, for a segment whose ordinals already are global.
    static SegmentToGlobalOrds identity();
    /// A mapping that adds the delta stored per ordinal.
    static SegmentToGlobalOrds from_deltas(PackedLongValues deltas);

    /// The global ordinal of `segment_ord`.
    [[nodiscard]] Result<int64_t> get(int64_t segment_ord) const;

    /// Whether this is the identity mapping.
    [[nodiscard]] bool is_identity() const noexcept { return deltas_ == nullptr; }

private:
    std::shared_ptr<const PackedLongValues> deltas_;
};

/// Maps per-segment ordinals to and from a global ordinal space.
///
/// Port of `OrdinalMap` (`core/index/ordinal_map.rs`).
///
/// Building the map merge-sorts the term dictionaries of every segment, which is
/// costly; when only one segment is involved its ordinals are already global.
///
/// **Deviations:** the reference implementation keeps the first-segment and delta
/// lookups in a two-alternative enum that swaps in a zero-filled `LongValues` for
/// the common "first segment holds everything" case, and keeps the per-segment
/// deltas either as packed ints or as monotonic packed longs; the port uses a
/// `nullptr` for the all-zero case and always keeps monotonic packed longs.  It
/// also only exposes `build` over [`TermsEnum`]s: the `build_from_sorted` /
/// `build_from_sorted_set` helpers need `SortedDocValues::terms_enum`, which the
/// port leaves unimplemented, so callers pass the enums in.
export class OrdinalMap {
public:
    /// Merges `subs` (one term enum per segment) into a global ordinal space;
    /// `weights` ideally reflect how many terms each segment introduces.
    [[nodiscard]] static Result<OrdinalMap> build(std::vector<std::unique_ptr<TermsEnum>> subs,
                                                  std::span<const int64_t> weights,
                                                  float acceptable_overhead_ratio);

    /// The number of unique terms in the global space.
    [[nodiscard]] int64_t value_count() const noexcept { return value_count_; }

    /// The ordinal of `global_ord` in the first segment that contains it.
    [[nodiscard]] Result<int64_t> get_first_segment_ord(int64_t global_ord) const;
    /// The index of the first segment that contains `global_ord`.
    [[nodiscard]] Result<size_t> get_first_segment_number(int64_t global_ord) const;
    /// The global ordinal of `segment_ord` in segment `segment_index`.
    [[nodiscard]] Result<int64_t> get_global_ord(size_t segment_index, int64_t segment_ord) const;
    /// The mapping of one segment, in the segment's original index order.
    [[nodiscard]] const SegmentToGlobalOrds &segment_to_global_ords(size_t segment_index) const;

    [[nodiscard]] const SegmentMap &segment_map() const noexcept { return segment_map_; }

private:
    OrdinalMap(int64_t value_count, std::optional<PackedLongValues> first_segments,
               std::optional<PackedLongValues> global_ord_deltas, SegmentMap segment_map,
               std::vector<SegmentToGlobalOrds> segment_to_global_ords);

    /// The value of `values`, or `0` when the map stored the all-zero shortcut.
    [[nodiscard]] static Result<int64_t> value_at(const std::optional<PackedLongValues> &values, int64_t index);

    int64_t value_count_{0};
    /// `global_ord -> first segment index`; absent when every value is zero.
    std::optional<PackedLongValues> first_segments_;
    /// `global_ord -> global_ord - first_segment_ord`; absent when every value is zero.
    std::optional<PackedLongValues> global_ord_deltas_;
    SegmentMap segment_map_;
    /// Indexed by the position in [`SegmentMap::new_to_old`], like the reference
    /// implementation's vector.
    std::vector<SegmentToGlobalOrds> segment_to_global_ords_;
};

} // namespace lucene
