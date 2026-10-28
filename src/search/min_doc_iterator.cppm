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

export module lucene_core:min_doc_iterator;

import :types;
import std;

import :doc_id_set_iterator;
import :error;

namespace lucene {

/// An iterator over `[segment_min_doc, max_doc)`.
///
/// Port of `MinDocIterator` (`search/comparators/min_doc_iterator.rs`): it skips
/// straight to the smallest live document of a segment on the first call, which
/// is what a leaf comparator needs to decide whether it has anything to do.
export class MinDocIterator final : public DocIdSetIterator {
public:
    MinDocIterator(int32_t segment_min_doc, int32_t max_doc)
        : segment_min_doc_(segment_min_doc), max_doc_(max_doc) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override {
        // **Deviation:** `doc_ + 1` overflows once the iterator is exhausted in
        // the reference implementation; here it simply stays exhausted.
        if (doc_ == NO_MORE_DOCS) {
            return NO_MORE_DOCS;
        }
        return advance(doc_ + 1);
    }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override {
        return static_cast<int64_t>(max_doc_) - static_cast<int64_t>(segment_min_doc_);
    }

private:
    int32_t segment_min_doc_;
    int32_t max_doc_;
    int32_t doc_{-1};
};

/// An [`Int32`] iterator over "no documents".
[[nodiscard]] inline std::unique_ptr<DocIdSetIterator> min_doc_iterator(int32_t segment_min_doc, int32_t max_doc) {
    return std::make_unique<MinDocIterator>(segment_min_doc, max_doc);
}

} // namespace lucene
