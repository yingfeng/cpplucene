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

export module lucene_core:union_postings;

import :types;
import std;

import :doc_id_set_iterator;
import :error;
import :postings_enum;

namespace lucene {

/// The logical union of several postings iterators, merging the positions of all
/// the iterators that are on the current document.
///
/// Port of `UnionPostingsEnum` (`core/search/multi_phrase_query.rs`).  It is what
/// makes a multi-phrase query a disjunction at one position: every term of the
/// position contributes its postings, and a document matches when any of them is
/// present.
///
/// The positions are merged while [`freq`] runs, so [`next_position`] must not be
/// called before it, exactly like the reference implementation.
export class UnionPostingsEnum final : public PostingsEnum {
public:
    /// The union of `subs`, which must not be empty.
    explicit UnionPostingsEnum(std::vector<std::unique_ptr<PostingsEnum>> subs);

    // ---- PostingsEnum ----

    [[nodiscard]] int32_t doc_id() const override;
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override { return cost_; }

    /// How many positions the current document has across all the sub postings;
    /// also fills the position queue [`next_position`] reads.
    [[nodiscard]] Result<int32_t> freq() override;
    [[nodiscard]] Result<int32_t> next_position() override;

    /// Always `-1`: merged postings carry no offsets.
    [[nodiscard]] Result<int32_t> start_offset() const override { return -1; }
    /// Always `-1`: merged postings carry no offsets.
    [[nodiscard]] Result<int32_t> end_offset() const override { return -1; }
    [[nodiscard]] Result<std::optional<std::span<const uint8_t>>> get_payload() const override {
        return std::optional<std::span<const uint8_t>>{};
    }

private:
    /// The sub postings with the smallest document, the root of the heap.
    [[nodiscard]] size_t top_index() const { return heap_.front(); }

    /// Restores the heap after the root's document changed, and returns the new
    /// root.
    [[nodiscard]] size_t update_top_index();

    [[nodiscard]] bool heap_greater(size_t left, size_t right) const {
        return subs_[left]->doc_id() > subs_[right]->doc_id();
    }

    std::vector<std::unique_ptr<PostingsEnum>> subs_;
    /// A min heap, by document, of the indices of [`subs`].
    std::vector<size_t> heap_;
    int64_t cost_{0};
    /// The merged positions of the current document, in ascending order.
    std::vector<int32_t> positions_;
    size_t position_index_{0};
    /// The document [`positions`] belongs to; `-2` is "no document yet".
    int32_t positions_doc_{-2};
};

} // namespace lucene
