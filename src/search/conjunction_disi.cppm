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

export module lucene_core:conjunction_disi;

import :types;

import std;

import :doc_id_set_iterator;
import :error;

namespace lucene {

/// Iterator over the documents that all of its sub-iterators contain.
///
/// Port of `ConjunctionDISI` (`core/search/conjunction_disi.rs`).
///
/// The two cheapest sub-iterators drive the intersection (advancing the
/// cheapest one first keeps the number of `advance` calls low), the others only
/// get asked when the two leads agree.
///
/// **Deviations** (C++ mechanics only):
///  * the reference implementation wraps the sub-iterators in `DocIdSetIteratorEnum2` (plain,
///    two-phase approximation or bit set) and picks the implementation at
///    construction; here [`DocIdSetIterator`] is already the abstract base, so a
///    conjunction just holds pointers and the bit set fast path
///    (`BitSetConjunctionDISI`) is not ported — nothing in the port produces bit
///    set iterators yet;
///  * the sub-iterators are **borrowed**: the caller keeps them alive for the
///    lifetime of the conjunction.
export class ConjunctionDISI final : public DocIdSetIterator {
public:
    /// Builds the conjunction of `iterators`, which must all be positioned on
    /// the same document and number at least two.
    [[nodiscard]] static Result<std::unique_ptr<ConjunctionDISI>>
    create(const std::vector<DocIdSetIterator *> &iterators);

    /// Conjunction of **owned** sub-iterators: `nullptr` for none, the element
    /// itself for one, and a conjunction otherwise.  This is the entry point the
    /// boolean scorer uses, because it has to hand over ownership anyway.
    [[nodiscard]] static Result<std::unique_ptr<DocIdSetIterator>>
    intersect(std::vector<std::unique_ptr<DocIdSetIterator>> iterators);

    [[nodiscard]] int32_t doc_id() const override { return iterators_[lead1_]->doc_id(); }

    [[nodiscard]] Result<int32_t> next_doc() override {
        auto doc = iterators_[lead1_]->next_doc();
        if (!doc.has_value()) {
            return doc;
        }
        return do_next(doc.value());
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        auto doc = iterators_[lead1_]->advance(target);
        if (!doc.has_value()) {
            return doc;
        }
        return do_next(doc.value());
    }

    [[nodiscard]] Result<int64_t> cost() const override { return iterators_[lead1_]->cost(); }

    /// The sub-iterators, in input order.
    [[nodiscard]] const std::vector<DocIdSetIterator *> &iterators() const noexcept { return iterators_; }

private:
    ConjunctionDISI(std::vector<DocIdSetIterator *> iterators, size_t lead1, size_t lead2, std::vector<size_t> others,
                    std::vector<std::unique_ptr<DocIdSetIterator>> owned = {})
        : iterators_(std::move(iterators)), lead1_(lead1), lead2_(lead2), others_(std::move(others)),
          owned_(std::move(owned)) {}

    /// Advances until every sub-iterator is on the same document as `doc`.
    [[nodiscard]] Result<int32_t> do_next(int32_t doc);

    std::vector<DocIdSetIterator *> iterators_;
    size_t lead1_;
    size_t lead2_;
    std::vector<size_t> others_;
    /// Sub-iterators this conjunction owns, if it created them (see [`intersect`]).
    std::vector<std::unique_ptr<DocIdSetIterator>> owned_;
};

} // namespace lucene
