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

export module lucene_core:filtered_doc_id_set_iterator;

import :types;
import std;

import :doc_id_set_iterator;
import :error;

namespace lucene {

/// A [`DocIdSetIterator`] that filters the documents of another one.
///
/// Port of the `FilteredDocIdSetIterator` trait
/// (`core/search/filtered_doc_id_set_iterator.rs`).
export class FilteredDocIdSetIterator : public DocIdSetIterator {
public:
    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override;

    [[nodiscard]] Result<int32_t> advance(int32_t target) override;

    [[nodiscard]] Result<int64_t> cost() const override;

protected:
    explicit FilteredDocIdSetIterator(std::unique_ptr<DocIdSetIterator> inner);

    /// Whether `doc` belongs to the result set.
    [[nodiscard]] virtual Result<bool> match(int32_t doc) = 0;

    /// The iterator being filtered.
    [[nodiscard]] DocIdSetIterator &inner() noexcept { return *inner_; }

private:
    std::unique_ptr<DocIdSetIterator> inner_;
    int32_t doc_{-1};
};

} // namespace lucene
