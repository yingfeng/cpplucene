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

export module lucene_core:docs_with_field_set;

import :types;
import std;

import :bits;
import :doc_id_set;
import :doc_id_set_iterator;
import :error;
import :fixed_bit_set;

namespace lucene {

/// [`DocIdSetIterator`] over a [`FixedBitSet`].
///
/// Port of `BitSetIterator` (`util/bit_set_iterator.rs`), reduced to what the
/// doc-values writers need (no bit-set access hooks).
export class FixedBitSetDISI final : public DocIdSetIterator {
public:
    explicit FixedBitSetDISI(std::shared_ptr<const FixedBitSet> set) : set_(std::move(set)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override { return move_to(doc_ + 1); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        return move_to(std::max(target, doc_ + 1));
    }
    [[nodiscard]] Result<int64_t> cost() const override {
        return static_cast<int64_t>(set_->cardinality());
    }

private:
    [[nodiscard]] Result<int32_t> move_to(int32_t target) {
        if (target < 0) {
            target = 0;
        }
        const auto found = set_->next_set_bit(static_cast<size_t>(target));
        doc_ = found < 0 ? NO_MORE_DOCS : static_cast<int32_t>(found);
        return doc_;
    }

    std::shared_ptr<const FixedBitSet> set_;
    int32_t doc_{-1};
};

/// Accumulator for the documents that have a value for a field.
///
/// Port of `DocsWithFieldSet` (`index/docs_with_field_set.rs`): as long as the
/// document ids arrive in order and without gaps, only the cardinality is
/// tracked (an [`AllDISI`]); the first gap materializes a [`FixedBitSet`].
///
/// **Deviation:** no RAM accounting (no `Accountable` in this port).
export class DocsWithFieldSet final : public DocIdSet {
public:
    /// Adds `doc_id`, which must be greater than every id added before.
    [[nodiscard]] VoidResult add(int32_t doc_id);

    /// Number of documents added so far.
    [[nodiscard]] int32_t cardinality() const noexcept { return cardinality_; }

    /// Requires [`finish`] to have been called first.
    [[nodiscard]] Result<std::unique_ptr<DocIdSetIterator>> iterator() const override;

    [[nodiscard]] VoidResult finish() override;

private:
    std::optional<FixedBitSet> set_;
    std::shared_ptr<const FixedBitSet> set_iter_;
    int32_t cardinality_{0};
    int32_t last_doc_id_{-1};
    bool finish_{false};
};

} // namespace lucene
