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

module lucene_core:ordinal_map.impl;

import :ordinal_map;

import std;

import :bytes_ref;
import :error;
import :packed_long_values;
import :priority_queue;
import :terms_enum;
import :types;

namespace lucene {

namespace {

/// A term enum tagged with the segment it belongs to.  Replaces the reference
/// implementation's generic `TermsEnumIndex`.
class TermsEnumIndex {
public:
    TermsEnumIndex(std::unique_ptr<TermsEnum> terms_enum, size_t sub_index)
            : terms_enum_(std::move(terms_enum)), sub_index_(sub_index) {}

    [[nodiscard]] TermsEnum *terms_enum() const noexcept { return terms_enum_.get(); }
    [[nodiscard]] size_t sub_index() const noexcept { return sub_index_; }

    /// Compares the current terms; a segment without a current term sorts last.
    [[nodiscard]] Result<int32_t> compare_term_to(const TermsEnumIndex &other) const {
        auto left = current_term();
        if (!left.has_value()) {
            return std::unexpected(std::move(left.error()));
        }
        auto right = other.current_term();
        if (!right.has_value()) {
            return std::unexpected(std::move(right.error()));
        }
        if (left.value() == nullptr) {
            return right.value() == nullptr ? 0 : 1;
        }
        if (right.value() == nullptr) {
            return -1;
        }
        return left.value()->compare_to(*right.value());
    }

    /// Whether the current term equals `term` (bytes, not the `BytesRef` object).
    [[nodiscard]] Result<bool> term_equals(const BytesRef &term) const {
        auto current = current_term();
        if (!current.has_value()) {
            return std::unexpected(std::move(current.error()));
        }
        if (current.value() == nullptr) {
            return false;
        }
        return current.value()->compare_to(term) == 0;
    }

private:
    [[nodiscard]] Result<const BytesRef *> current_term() const {
        if (terms_enum_ == nullptr) {
            return static_cast<const BytesRef *>(nullptr);
        }
        return terms_enum_->term();
    }

    std::unique_ptr<TermsEnum> terms_enum_;
    size_t sub_index_;
};

/// Orders the queued term enums by their current term.
class TermsEnumCompare {
public:
    explicit TermsEnumCompare(const std::vector<TermsEnumIndex> *indices) : indices_(indices) {}

    [[nodiscard]] Result<bool> less_than(size_t left, size_t right) const {
        auto order = (*indices_)[left].compare_term_to((*indices_)[right]);
        if (!order.has_value()) {
            return std::unexpected(std::move(order.error()));
        }
        return order.value() < 0;
    }

private:
    const std::vector<TermsEnumIndex> *indices_;
};

/// Advances `index` to its next term; false when the enum is exhausted.
[[nodiscard]] Result<bool> advance(TermsEnumIndex &index) {
    TermsEnum *terms_enum = index.terms_enum();
    if (terms_enum == nullptr) {
        return false;
    }
    auto next = terms_enum->next();
    if (!next.has_value()) {
        return std::unexpected(std::move(next.error()));
    }
    return next.value();
}

/// The ordinal a term enum currently points at.
[[nodiscard]] Result<int64_t> current_ord(const TermsEnumIndex &index) {
    TermsEnum *terms_enum = index.terms_enum();
    if (terms_enum == nullptr) {
        return err(LuceneError::illegal_state("segment terms enum is missing"));
    }
    return terms_enum->ord();
}

} // namespace

// ---- SegmentMap ----

Result<SegmentMap> SegmentMap::create(std::span<const int64_t> weights) {
    std::vector<int32_t> new_to_old(weights.size());
    std::iota(new_to_old.begin(), new_to_old.end(), int32_t{0});
    // Descending weight; a stable sort keeps the segment order for equal weights.
    std::ranges::stable_sort(new_to_old, [weights](int32_t left, int32_t right) {
        return weights[static_cast<size_t>(left)] > weights[static_cast<size_t>(right)];
    });

    SegmentMap map;
    map.new_to_old_ = std::move(new_to_old);
    map.old_to_new_.assign(weights.size(), 0);
    for (size_t position = 0; position < map.new_to_old_.size(); ++position) {
        map.old_to_new_[static_cast<size_t>(map.new_to_old_[position])] = static_cast<int32_t>(position);
    }
    return map;
}

size_t SegmentMap::new_to_old(size_t new_index) const {
    return static_cast<size_t>(new_to_old_[new_index]);
}

size_t SegmentMap::old_to_new(size_t old_index) const {
    return static_cast<size_t>(old_to_new_[old_index]);
}

// ---- SegmentToGlobalOrds ----

SegmentToGlobalOrds SegmentToGlobalOrds::identity() {
    return SegmentToGlobalOrds{};
}

SegmentToGlobalOrds SegmentToGlobalOrds::from_deltas(PackedLongValues deltas) {
    SegmentToGlobalOrds map;
    map.deltas_ = std::make_shared<const PackedLongValues>(std::move(deltas));
    return map;
}

Result<int64_t> SegmentToGlobalOrds::get(int64_t segment_ord) const {
    if (segment_ord < 0) {
        return err(LuceneError::illegal_argument("ordinal must not be negative: " + std::to_string(segment_ord)));
    }
    if (deltas_ == nullptr) {
        return segment_ord;
    }
    auto delta = deltas_->get(static_cast<size_t>(segment_ord));
    if (!delta.has_value()) {
        return std::unexpected(std::move(delta.error()));
    }
    return segment_ord + delta.value();
}

// ---- OrdinalMap ----

OrdinalMap::OrdinalMap(int64_t value_count, std::optional<PackedLongValues> first_segments,
                       std::optional<PackedLongValues> global_ord_deltas, SegmentMap segment_map,
                       std::vector<SegmentToGlobalOrds> segment_to_global_ords)
        : value_count_(value_count),
          first_segments_(std::move(first_segments)),
          global_ord_deltas_(std::move(global_ord_deltas)),
          segment_map_(std::move(segment_map)),
          segment_to_global_ords_(std::move(segment_to_global_ords)) {}

Result<int64_t> OrdinalMap::value_at(const std::optional<PackedLongValues> &values, int64_t index) {
    if (index < 0) {
        return err(LuceneError::illegal_argument("ordinal must not be negative: " + std::to_string(index)));
    }
    if (!values.has_value()) {
        // The all-zero shortcut.
        return int64_t{0};
    }
    return values->get(static_cast<size_t>(index));
}

Result<OrdinalMap> OrdinalMap::build(std::vector<std::unique_ptr<TermsEnum>> subs, std::span<const int64_t> weights,
                                    float acceptable_overhead_ratio) {
    if (subs.size() != weights.size()) {
        return err(LuceneError::illegal_argument("subs and weights must have the same length"));
    }
    const size_t sub_len = subs.size();
    if (sub_len == 0) {
        return err(LuceneError::illegal_argument("at least one sub is required"));
    }

    auto segment_map = SegmentMap::create(weights);
    if (!segment_map.has_value()) {
        return std::unexpected(std::move(segment_map.error()));
    }

    // One monotonic accumulator per segment, plus the accumulators for the global
    // lookup tables.
    std::vector<PackedLongValues::Builder> ord_deltas;
    ord_deltas.reserve(sub_len);
    for (size_t i = 0; i < sub_len; ++i) {
        ord_deltas.emplace_back(PackedLongValues::DEFAULT_PAGE_SIZE, 64, 0, true);
    }
    PackedLongValues::Builder first_segments(PackedLongValues::DEFAULT_PAGE_SIZE, 64, 0, true);
    PackedLongValues::Builder global_ord_deltas(PackedLongValues::DEFAULT_PAGE_SIZE, 64, 0, true);

    std::vector<int64_t> ord_delta_bits(sub_len, 0);
    std::vector<int64_t> segment_ords(sub_len, 0);
    int64_t first_segment_bits = 0;
    bool ord_delta_bits_has_value = false;

    // Sort the segments by descending weight and pull a term enum over each.
    std::vector<std::optional<std::unique_ptr<TermsEnum>>> subs_with_holes;
    subs_with_holes.reserve(sub_len);
    for (auto &sub : subs) {
        subs_with_holes.emplace_back(std::move(sub));
    }
    std::vector<TermsEnumIndex> indices;
    indices.reserve(sub_len);
    for (size_t i = 0; i < sub_len; ++i) {
        const size_t mapped = segment_map.value().new_to_old(i);
        TermsEnumIndex index(std::move(subs_with_holes[mapped].value()), i);
        auto advanced = advance(index);
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        if (advanced.value()) {
            indices.push_back(std::move(index));
        }
    }

    auto queue = PriorityQueue<size_t, TermsEnumCompare>::create(sub_len, TermsEnumCompare(&indices));
    if (!queue.has_value()) {
        return std::unexpected(std::move(queue.error()));
    }
    for (size_t i = 0; i < indices.size(); ++i) {
        auto added = queue.value().add(i);
        if (!added.has_value()) {
            return std::unexpected(std::move(added.error()));
        }
    }

    int64_t global_ord = 0;
    BytesRef top_term;
    while (queue.value().size() != 0) {
        size_t top_index = *queue.value().top();
        {
            const BytesRef *term = indices[top_index].terms_enum()->term().value();
            if (term != nullptr) {
                top_term = *term;
            } else {
                top_term = BytesRef();
            }
        }

        int32_t first_segment_index = std::numeric_limits<int32_t>::max();
        int64_t global_ord_delta = std::numeric_limits<int64_t>::max();
        // Walk past every segment that holds this term, recording the per-segment
        // ord deltas on the way.
        while (true) {
            auto segment_ord = current_ord(indices[top_index]);
            if (!segment_ord.has_value()) {
                return std::unexpected(std::move(segment_ord.error()));
            }
            const int64_t delta = global_ord - segment_ord.value();
            const size_t segment_index = indices[top_index].sub_index();
            // The lowest segment that holds the term is kept: if it holds most of
            // the values, the deltas stay small.
            if (static_cast<int32_t>(segment_index) < first_segment_index) {
                first_segment_index = static_cast<int32_t>(segment_index);
                global_ord_delta = delta;
            }
            ord_delta_bits[segment_index] |= delta;
            ord_delta_bits_has_value = true;

            // A term enum may skip ordinals, so fill the gap explicitly.
            while (segment_ords[segment_index] <= segment_ord.value()) {
                auto added = ord_deltas[segment_index].add(delta);
                if (!added.has_value()) {
                    return std::unexpected(std::move(added.error()));
                }
                segment_ords[segment_index] += 1;
            }

            auto advanced = advance(indices[top_index]);
            if (!advanced.has_value()) {
                return std::unexpected(std::move(advanced.error()));
            }
            if (!advanced.value()) {
                auto popped = queue.value().pop();
                if (!popped.has_value()) {
                    return std::unexpected(std::move(popped.error()));
                }
                if (queue.value().size() == 0) {
                    break;
                }
                top_index = *queue.value().top();
            } else {
                auto updated = queue.value().update_top();
                if (!updated.has_value()) {
                    return std::unexpected(std::move(updated.error()));
                }
                top_index = *updated.value();
            }
            auto equals = indices[top_index].term_equals(top_term);
            if (!equals.has_value()) {
                return std::unexpected(std::move(equals.error()));
            }
            if (!equals.value()) {
                break;
            }
        }

        {
            auto added = first_segments.add(first_segment_index);
            if (!added.has_value()) {
                return std::unexpected(std::move(added.error()));
            }
            first_segment_bits |= first_segment_index;
            auto delta_added = global_ord_deltas.add(global_ord_delta);
            if (!delta_added.has_value()) {
                return std::unexpected(std::move(delta_added.error()));
            }
            global_ord += 1;
        }
    }

    const int64_t value_count = global_ord;

    // When the first segment holds every term, both lookup tables are all zeroes
    // and the reference implementation swaps in a zero-filled `LongValues`.
    std::optional<PackedLongValues> first_segments_values;
    std::optional<PackedLongValues> global_ord_deltas_values;
    if (!(ord_delta_bits_has_value && ord_delta_bits[0] == 0 && first_segment_bits == 0)) {
        auto built_first = first_segments.build();
        if (!built_first.has_value()) {
            return std::unexpected(std::move(built_first.error()));
        }
        first_segments_values = std::move(built_first.value());
        auto built_deltas = global_ord_deltas.build();
        if (!built_deltas.has_value()) {
            return std::unexpected(std::move(built_deltas.error()));
        }
        global_ord_deltas_values = std::move(built_deltas.value());
    }

    // Build the per-segment mappings, in the segment order of `SegmentMap`.
    std::vector<SegmentToGlobalOrds> segment_to_global;
    segment_to_global.reserve(sub_len);
    for (size_t i = 0; i < ord_deltas.size(); ++i) {
        auto deltas = ord_deltas[i].build();
        if (!deltas.has_value()) {
            return std::unexpected(std::move(deltas.error()));
        }
        if (ord_delta_bits[i] == 0) {
            // The segment's ordinals match the global ones: low cardinality in a
            // large segment makes that the common case.
            segment_to_global.push_back(SegmentToGlobalOrds::identity());
        } else {
            segment_to_global.push_back(SegmentToGlobalOrds::from_deltas(std::move(deltas.value())));
        }
    }

    return OrdinalMap(value_count, std::move(first_segments_values), std::move(global_ord_deltas_values),
                      std::move(segment_map.value()), std::move(segment_to_global));
}

Result<int64_t> OrdinalMap::get_first_segment_ord(int64_t global_ord) const {
    if (global_ord < 0 || global_ord >= value_count_) {
        return err(LuceneError::illegal_argument("global ordinal out of range: " + std::to_string(global_ord)));
    }
    auto delta = value_at(global_ord_deltas_, global_ord);
    if (!delta.has_value()) {
        return std::unexpected(std::move(delta.error()));
    }
    return global_ord - delta.value();
}

Result<size_t> OrdinalMap::get_first_segment_number(int64_t global_ord) const {
    if (global_ord < 0 || global_ord >= value_count_) {
        return err(LuceneError::illegal_argument("global ordinal out of range: " + std::to_string(global_ord)));
    }
    auto first = value_at(first_segments_, global_ord);
    if (!first.has_value()) {
        return std::unexpected(std::move(first.error()));
    }
    if (first.value() < 0 || static_cast<size_t>(first.value()) >= segment_map_.size()) {
        return err(LuceneError::illegal_argument("segment index out of range: " + std::to_string(first.value())));
    }
    return segment_map_.new_to_old(static_cast<size_t>(first.value()));
}

Result<int64_t> OrdinalMap::get_global_ord(size_t segment_index, int64_t segment_ord) const {
    const SegmentToGlobalOrds &map = segment_to_global_ords(segment_index);
    return map.get(segment_ord);
}

const SegmentToGlobalOrds &OrdinalMap::segment_to_global_ords(size_t segment_index) const {
    const size_t mapped = segment_map_.old_to_new(segment_index);
    return segment_to_global_ords_[mapped];
}

} // namespace lucene
