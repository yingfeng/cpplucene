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

export module lucene_core:merged_iterator;

import :error;
import :types;

import std;

namespace lucene {

/// A pull iterator over items of type `T`; the hook [`MergedIterator`] needs.
///
/// Port of the reference implementation's `IteratorExt` (`util/iterator.rs`),
/// reduced to the two methods `MergedIterator` uses.
export template <typename T>
class IteratorExt {
public:
    virtual ~IteratorExt() = default;

    /// Whether [`next`] has an item to return.
    [[nodiscard]] virtual Result<bool> has_next() = 0;

    /// The next item, or `std::nullopt` when the iterator is exhausted.
    [[nodiscard]] virtual Result<std::optional<T>> next() = 0;
};

/// A merged, sorted view over several sorted iterators.
///
/// If `remove_duplicates` is set and an item appears in several iterators, it is
/// returned once, i.e. the result is the sorted union.  Otherwise every item of
/// every iterator is returned, in an unspecified order among equal items.
///
/// Port of `util/merged_iterator.rs`.
///
/// **Caveats** (inherited from the reference implementation): the iterators
/// must really be sorted, and when `remove_duplicates` is set, duplicates
/// *within* one iterator are not removed.
///
/// **Deviation:** the reference implementation keeps its heap in
/// `PriorityQueue`; this port uses `std::push_heap`/`std::pop_heap` over the same
/// `(current item, iterator index)` ordering, because the items have to be
/// compared directly and the port's `PriorityQueue` compares through a
/// comparator object.
export template <typename T>
class MergedIterator final : public IteratorExt<T> {
public:
    /// Merges `iterators`, removing duplicates.
    [[nodiscard]] static Result<std::unique_ptr<MergedIterator>> create(
            std::vector<std::unique_ptr<IteratorExt<T>>> iterators) {
        return create_with_remove_duplicates(true, std::move(iterators));
    }

    /// Merges `iterators`, removing duplicates only when asked to.
    [[nodiscard]] static Result<std::unique_ptr<MergedIterator>> create_with_remove_duplicates(
            bool remove_duplicates, std::vector<std::unique_ptr<IteratorExt<T>>> iterators) {
        std::unique_ptr<MergedIterator> merged(new MergedIterator(remove_duplicates));
        merged->subs_.reserve(iterators.size());
        for (size_t index = 0; index < iterators.size(); ++index) {
            Sub sub;
            sub.iterator = std::move(iterators[index]);
            sub.index = index;
            auto has = sub.iterator->has_next();
            if (!has.has_value()) {
                return std::unexpected(std::move(has.error()));
            }
            if (has.value()) {
                auto first = sub.iterator->next();
                if (!first.has_value()) {
                    return std::unexpected(std::move(first.error()));
                }
                if (!first.value().has_value()) {
                    return err(LuceneError::illegal_state("an iterator reported has_next but no next item"));
                }
                sub.current = std::move(first.value());
            }
            merged->subs_.push_back(std::move(sub));
        }
        merged->top_.assign(merged->subs_.size(), 0);
        for (size_t index = 0; index < merged->subs_.size(); ++index) {
            if (merged->subs_[index].current.has_value()) {
                merged->heap_.push_back(index);
            }
        }
        merged->heap_cmp_ = HeapCompare{&merged->subs_};
        std::make_heap(merged->heap_.begin(), merged->heap_.end(), merged->heap_cmp_);
        return merged;
    }

    [[nodiscard]] Result<bool> has_next() override {
        if (!heap_.empty()) {
            return true;
        }
        for (size_t i = 0; i < num_top_; ++i) {
            auto has = subs_[top_[i]].iterator->has_next();
            if (!has.has_value()) {
                return std::unexpected(std::move(has.error()));
            }
            if (has.value()) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] Result<std::optional<T>> next() override {
        if (auto pushed = push_top(); !pushed.has_value()) {
            return std::unexpected(std::move(pushed.error()));
        }

        if (!heap_.empty()) {
            if (auto pulled = pull_top(); !pulled.has_value()) {
                return std::unexpected(std::move(pulled.error()));
            }
        } else {
            current_.reset();
        }

        if (!current_.has_value()) {
            return err(LuceneError::no_such_element("no such element"));
        }
        std::optional<T> result = std::move(current_);
        current_.reset();
        return result;
    }

private:
    struct Sub {
        std::unique_ptr<IteratorExt<T>> iterator;
        std::optional<T> current;
        size_t index{0};
    };

    struct HeapCompare {
        const std::vector<Sub> *subs{nullptr};

        /// Orders the heap so that its top is the *smallest* `(current, index)`.
        bool operator()(size_t a, size_t b) const {
            const T &left = *(*subs)[a].current;
            const T &right = *(*subs)[b].current;
            if (left < right) {
                return false;
            }
            if (right < left) {
                return true;
            }
            return (*subs)[a].index > (*subs)[b].index;
        }
    };

    explicit MergedIterator(bool remove_duplicates) : remove_duplicates_(remove_duplicates) {}

    /// Moves every iterator of the current top group on, and puts it back in the
    /// heap.
    [[nodiscard]] VoidResult push_top() {
        for (size_t i = 0; i < num_top_; ++i) {
            Sub &sub = subs_[top_[i]];
            auto has = sub.iterator->has_next();
            if (!has.has_value()) {
                return std::unexpected(std::move(has.error()));
            }
            if (has.value()) {
                auto item = sub.iterator->next();
                if (!item.has_value()) {
                    return std::unexpected(std::move(item.error()));
                }
                sub.current = std::move(item.value());
                heap_.push_back(sub.index);
                std::push_heap(heap_.begin(), heap_.end(), heap_cmp_);
            } else {
                sub.current.reset();
            }
        }
        num_top_ = 0;
        return {};
    }

    /// Pops the smallest item together with every other item that equals it.
    [[nodiscard]] VoidResult pull_top() {
        size_t first = heap_.front();
        std::pop_heap(heap_.begin(), heap_.end(), heap_cmp_);
        heap_.pop_back();
        top_[num_top_] = first;
        num_top_ = 1;

        if (remove_duplicates_) {
            // Collect the iterators whose current item equals the top one.
            while (!heap_.empty()) {
                size_t candidate = heap_.front();
                if (!(*subs_[candidate].current == *subs_[first].current)) {
                    break;
                }
                std::pop_heap(heap_.begin(), heap_.end(), heap_cmp_);
                heap_.pop_back();
                top_[num_top_] = candidate;
                ++num_top_;
            }
        }

        current_ = std::move(subs_[first].current);
        subs_[first].current.reset();
        return {};
    }

    std::vector<Sub> subs_;
    std::vector<size_t> heap_;
    std::vector<size_t> top_;
    HeapCompare heap_cmp_;
    bool remove_duplicates_;
    size_t num_top_{0};
    std::optional<T> current_;
};

} // namespace lucene
