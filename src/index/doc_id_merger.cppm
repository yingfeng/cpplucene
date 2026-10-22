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

export module lucene_core:doc_id_merger;

import :types;
import std;

import :doc_id_set_iterator;
import :doc_map;
import :error;
import :priority_queue;

namespace lucene {

/// One sub-reader taking part in a merge.
///
/// Port of the reference implementation's `Sub<S>`.  `S` must provide
///  * `Result<int32_t> next_doc()` -- the next raw doc id of that sub-reader, or
///    `NO_MORE_DOCS`;
///  * `const DocMap &get_doc_map() const` -- the map into the merged segment.
export template <class S>
class DocIdMergerSub {
public:
    explicit DocIdMergerSub(S sub) : sub(std::move(sub)) {}

    /// Like `next_doc()` but skips unmapped documents and returns the next
    /// *mapped* doc id, or [`NO_MORE_DOCS`].
    ///
    /// As a side effect this stores the result in [`mapped_doc_id`].
    [[nodiscard]] Result<int32_t> next_mapped_doc() {
        while (true) {
            auto doc = sub.next_doc();
            if (!doc.has_value()) {
                return std::unexpected(std::move(doc.error()));
            }
            if (doc.value() == NO_MORE_DOCS) {
                mapped_doc_id = NO_MORE_DOCS;
                return NO_MORE_DOCS;
            }
            auto mapped = sub.get_doc_map().get(doc.value());
            if (!mapped.has_value()) {
                return std::unexpected(std::move(mapped.error()));
            }
            if (mapped.value() != -1) {
                mapped_doc_id = mapped.value();
                return mapped_doc_id;
            }
        }
    }

    /// The wrapped sub-reader.
    S sub;
    /// Mapped doc id of the current document.
    int32_t mapped_doc_id{0};
};

/// Compares two subs by their current mapped doc id, keeping the subs alive.
export template <class S>
class DocIdMergerSubCompare {
public:
    explicit DocIdMergerSubCompare(std::shared_ptr<std::vector<DocIdMergerSub<S>>> subs)
            : subs_(std::move(subs)) {}

    [[nodiscard]] Result<bool> less_than(const size_t &a, const size_t &b) const {
        return subs_->operator[](a).mapped_doc_id < subs_->operator[](b).mapped_doc_id;
    }

    [[nodiscard]] const std::vector<DocIdMergerSub<S>> &subs() const { return *subs_; }
    [[nodiscard]] std::vector<DocIdMergerSub<S>> &subs_mut() { return *subs_; }

private:
    std::shared_ptr<std::vector<DocIdMergerSub<S>>> subs_;
};

/// Merges the subs one after the other: every document of sub 0, then sub 1, ...
///
/// Port of `SequentialDocIDMerger`.  This is what Lucene uses when the index is
/// not sorted, because the merged doc ids are monotonically increasing per sub
/// and simply concatenating them keeps them increasing.
export template <class S>
class SequentialDocIdMerger {
public:
    [[nodiscard]] static Result<SequentialDocIdMerger> create(std::vector<DocIdMergerSub<S>> subs) {
        SequentialDocIdMerger merger(std::make_shared<std::vector<DocIdMergerSub<S>>>(std::move(subs)));
        if (auto reset = merger.reset(); !reset.has_value()) {
            return std::unexpected(std::move(reset.error()));
        }
        return merger;
    }

    /// Restarts the iteration from the first sub.
    [[nodiscard]] VoidResult reset() {
        if (!subs_->empty()) {
            current_ = 0;
            next_index_ = 1;
        } else {
            current_.reset();
            next_index_ = 0;
        }
        return {};
    }

    /// The index of the sub that holds the next document, or `std::nullopt` when
    /// everything has been consumed.
    [[nodiscard]] Result<std::optional<size_t>> next() {
        while (true) {
            if (!current_.has_value()) {
                return err(LuceneError::illegal_state("current is None"));
            }
            const size_t current = current_.value();
            auto next_mapped_doc = (*subs_)[current].next_mapped_doc();
            if (!next_mapped_doc.has_value()) {
                return std::unexpected(std::move(next_mapped_doc.error()));
            }
            if (next_mapped_doc.value() != NO_MORE_DOCS) {
                return std::optional<size_t>(current);
            }
            if (next_index_ == subs_->size()) {
                current_.reset();
                return std::optional<size_t>{};
            }
            current_ = next_index_;
            ++next_index_;
        }
    }

    [[nodiscard]] std::vector<DocIdMergerSub<S>> &subs() { return *subs_; }
    [[nodiscard]] const std::vector<DocIdMergerSub<S>> &subs() const { return *subs_; }

private:
    explicit SequentialDocIdMerger(std::shared_ptr<std::vector<DocIdMergerSub<S>>> subs) : subs_(std::move(subs)) {}

    std::shared_ptr<std::vector<DocIdMergerSub<S>>> subs_;
    std::optional<size_t> current_;
    size_t next_index_{0};
};

/// Merges the subs by always picking the smallest mapped doc id.
///
/// Port of `SortedDocIDMerger`, used when the index carries a sort order so the
/// merged segment must keep documents ordered.  Sub 0 is used directly until
/// another sub overtakes it; the rest wait in a min-heap.
export template <class S>
class SortedDocIdMerger {
public:
    /// `max_count` is the number of subs; it must be greater than one.
    [[nodiscard]] static Result<SortedDocIdMerger> create(std::vector<DocIdMergerSub<S>> subs, size_t max_count) {
        if (max_count <= 1) {
            return err(LuceneError::illegal_argument("max_count must be greater than 1"));
        }
        auto shared = std::make_shared<std::vector<DocIdMergerSub<S>>>(std::move(subs));
        auto queue = PriorityQueue<size_t, DocIdMergerSubCompare<S>>::create(
                max_count - 1, DocIdMergerSubCompare<S>(shared));
        if (!queue.has_value()) {
            return std::unexpected(std::move(queue.error()));
        }
        SortedDocIdMerger merger(std::move(shared), std::move(queue.value()));
        if (auto reset = merger.reset(); !reset.has_value()) {
            return std::unexpected(std::move(reset.error()));
        }
        return merger;
    }

    /// Restarts the iteration; the caller may not have drained the queue.
    [[nodiscard]] VoidResult reset() {
        auto &subs = *subs_;
        queue_.clear();
        current_.reset();
        if (!subs.empty()) {
            // Setting mapped_doc_id = -1 guarantees this entry is the top of the
            // queue, so the first `next()` advances it.
            subs[0].mapped_doc_id = -1;
            current_ = 0;
        }
        // Keep the queue empty until every sub has advanced successfully.
        for (size_t i = 1; i < subs.size(); ++i) {
            auto next = subs[i].next_mapped_doc();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
            if (subs[i].mapped_doc_id != NO_MORE_DOCS) {
                auto added = queue_.add(i);
                if (!added.has_value()) {
                    return std::unexpected(std::move(added.error()));
                }
            }
        }
        return set_queue_min_doc_id();
    }

    /// The index of the sub that holds the next document, or `std::nullopt` when
    /// everything has been consumed.
    [[nodiscard]] Result<std::optional<size_t>> next() {
        if (!current_.has_value()) {
            return err(LuceneError::illegal_state("current is None"));
        }
        const size_t current = current_.value();
        auto next_doc = (*subs_)[current].next_mapped_doc();
        if (!next_doc.has_value()) {
            return std::unexpected(std::move(next_doc.error()));
        }

        if (next_doc.value() < queue_min_doc_id_) {
            // The common case when index sorting is disabled or correlates with
            // the doc order: the current sub still wins.
            return std::optional<size_t>(current);
        }

        if (next_doc.value() == NO_MORE_DOCS) {
            if (queue_.size() == 0) {
                current_.reset();
            } else {
                auto popped = queue_.pop();
                if (!popped.has_value()) {
                    return std::unexpected(std::move(popped.error()));
                }
                current_ = popped.value();
            }
        } else if (queue_.size() > 0) {
            const size_t *top = queue_.top();
            if (top == nullptr) {
                return err(LuceneError::illegal_state("no top available"));
            }
            const size_t new_current = *top;
            auto updated = queue_.update_top_with_new_top(current);
            if (!updated.has_value()) {
                return std::unexpected(std::move(updated.error()));
            }
            current_ = new_current;
        }

        auto min_doc_id = set_queue_min_doc_id();
        if (!min_doc_id.has_value()) {
            return std::unexpected(std::move(min_doc_id.error()));
        }
        if (!current_.has_value()) {
            return std::optional<size_t>{};
        }
        return std::optional<size_t>(current_.value());
    }

    [[nodiscard]] std::vector<DocIdMergerSub<S>> &subs() { return *subs_; }
    [[nodiscard]] const std::vector<DocIdMergerSub<S>> &subs() const { return *subs_; }

private:
    SortedDocIdMerger(std::shared_ptr<std::vector<DocIdMergerSub<S>>> subs,
                      PriorityQueue<size_t, DocIdMergerSubCompare<S>> queue)
            : subs_(std::move(subs)), queue_(std::move(queue)) {}

    [[nodiscard]] VoidResult set_queue_min_doc_id() {
        if (queue_.size() > 0) {
            const size_t *top = queue_.top();
            if (top == nullptr) {
                return err(LuceneError::illegal_state("no top available"));
            }
            queue_min_doc_id_ = (*subs_)[*top].mapped_doc_id;
        } else {
            queue_min_doc_id_ = NO_MORE_DOCS;
        }
        return {};
    }

    std::shared_ptr<std::vector<DocIdMergerSub<S>>> subs_;
    PriorityQueue<size_t, DocIdMergerSubCompare<S>> queue_;
    std::optional<size_t> current_;
    int32_t queue_min_doc_id_{0};
};

/// Reuse API used by postings (and by `MappedMultiFields`) during a merge.
///
/// Port of the reference implementation's `DocIDMerger` trait plus `DocIDMergerEnum`: the enum is a
/// `std::variant` here.
export template <class S>
class DocIdMerger {
public:
    /// Builds a merger over `subs`, using `max_count` as the heap capacity.
    [[nodiscard]] static Result<DocIdMerger> of_with_max_count(std::vector<DocIdMergerSub<S>> subs, size_t max_count,
                                                               bool index_is_sorted) {
        if (index_is_sorted && max_count > 1) {
            auto sorted = SortedDocIdMerger<S>::create(std::move(subs), max_count);
            if (!sorted.has_value()) {
                return std::unexpected(std::move(sorted.error()));
            }
            return DocIdMerger(std::move(sorted.value()));
        }
        auto sequential = SequentialDocIdMerger<S>::create(std::move(subs));
        if (!sequential.has_value()) {
            return std::unexpected(std::move(sequential.error()));
        }
        return DocIdMerger(std::move(sequential.value()));
    }

    /// Builds a merger over `subs`.
    [[nodiscard]] static Result<DocIdMerger> of(std::vector<DocIdMergerSub<S>> subs, bool index_is_sorted) {
        const size_t max_count = subs.size();
        return of_with_max_count(std::move(subs), max_count, index_is_sorted);
    }

    /// Restarts the iteration.
    [[nodiscard]] VoidResult reset() {
        return std::visit([](auto &merger) { return merger.reset(); }, inner_);
    }

    /// The index of the sub holding the next document, or `std::nullopt` at the
    /// end.  The iterator must not be used after it returned `std::nullopt`.
    [[nodiscard]] Result<std::optional<size_t>> next() {
        return std::visit([](auto &merger) { return merger.next(); }, inner_);
    }

    [[nodiscard]] std::vector<DocIdMergerSub<S>> &subs() {
        return std::visit([](auto &merger) -> std::vector<DocIdMergerSub<S>> & { return merger.subs(); }, inner_);
    }

    [[nodiscard]] const std::vector<DocIdMergerSub<S>> &subs() const {
        return std::visit([](const auto &merger) -> const std::vector<DocIdMergerSub<S>> & { return merger.subs(); },
                          inner_);
    }

private:
    explicit DocIdMerger(SequentialDocIdMerger<S> merger) : inner_(std::move(merger)) {}
    explicit DocIdMerger(SortedDocIdMerger<S> merger) : inner_(std::move(merger)) {}

    std::variant<SequentialDocIdMerger<S>, SortedDocIdMerger<S>> inner_;
};

} // namespace lucene
