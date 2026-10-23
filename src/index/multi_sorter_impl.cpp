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

module lucene_core:multi_sorter.impl;

import :multi_sorter;

import std;

import :bits;
import :codec_reader;
import :error;
import :index_sorter;
import :merge_state;
import :packed_long_values;
import :priority_queue;
import :sort_field;
import :types;

namespace lucene {

namespace {

/// The merge-sort cursor of one segment: the current document and its sort keys.
struct LeafAndDocId {
    size_t reader_index{0};
    /// Borrowed from the reader.
    const Bits *live_docs{nullptr};
    int32_t max_doc{0};
    std::vector<int64_t> values_as_comparable_longs;
    int32_t doc_id{0};
};

/// Orders the cursors by their sort keys, then by reader and document id.
class LeafAndDocIdCompare {
public:
    LeafAndDocIdCompare(size_t comparables_len, const std::vector<int32_t> *reverse_muls)
            : comparables_len_(comparables_len), reverse_muls_(reverse_muls) {}

    [[nodiscard]] Result<bool> less_than(const LeafAndDocId &left, const LeafAndDocId &right) const {
        for (size_t i = 0; i < comparables_len_; ++i) {
            const int64_t a = left.values_as_comparable_longs[i];
            const int64_t b = right.values_as_comparable_longs[i];
            if (a == b) {
                continue;
            }
            const int32_t order = a < b ? -1 : 1;
            return (*reverse_muls_)[i] * order < 0;
        }
        // Documents that compare equal keep their reader order, then their document
        // order, which is what makes the sort stable.
        if (left.reader_index != right.reader_index) {
            return left.reader_index < right.reader_index;
        }
        return left.doc_id < right.doc_id;
    }

private:
    size_t comparables_len_;
    const std::vector<int32_t> *reverse_muls_;
};

/// Whether `doc_id` of the cursor is a live document.
[[nodiscard]] Result<bool> is_live(const LeafAndDocId &cursor) {
    if (cursor.live_docs == nullptr) {
        return true;
    }
    return cursor.live_docs->get(static_cast<size_t>(cursor.doc_id));
}

/// Refreshes the sort keys of `cursor` at its current document.
[[nodiscard]] VoidResult refresh_values(LeafAndDocId &cursor,
                                        const std::vector<std::vector<std::unique_ptr<ComparableProvider>>> &providers) {
    for (size_t field = 0; field < providers.size(); ++field) {
        auto *provider = providers[field][cursor.reader_index].get();
        if (provider == nullptr) {
            return err(LuceneError::illegal_state("index sorter did not return a comparable provider"));
        }
        auto value = provider->get_as_comparable_long(cursor.doc_id);
        if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
        }
        cursor.values_as_comparable_longs[field] = value.value();
    }
    return {};
}

} // namespace

Result<std::optional<std::vector<std::shared_ptr<MergeStateDocMap>>>> MultiSorter::sort(
        const Sort &sort, const std::vector<std::shared_ptr<CodecReader>> &readers) {
    const std::vector<SortField> &fields = sort.fields();
    if (readers.empty()) {
        return err(LuceneError::illegal_argument("at least one reader is required"));
    }

    // One provider per (field, reader) pair.
    std::vector<std::vector<std::unique_ptr<ComparableProvider>>> providers;
    std::vector<int32_t> reverse_muls;
    providers.reserve(fields.size());
    reverse_muls.reserve(fields.size());
    for (const SortField &field : fields) {
        auto sorter = field.index_sorter();
        if (!sorter.has_value()) {
            return std::unexpected(std::move(sorter.error()));
        }
        std::vector<std::unique_ptr<ComparableProvider>> per_reader;
        per_reader.reserve(readers.size());
        for (const auto &reader : readers) {
            if (reader == nullptr) {
                return err(LuceneError::illegal_argument("the readers must not contain null"));
            }
            auto provider = sorter.value()->comparable_provider(*reader);
            if (!provider.has_value()) {
                return std::unexpected(std::move(provider.error()));
            }
            per_reader.push_back(std::move(provider.value()));
        }
        providers.push_back(std::move(per_reader));
        reverse_muls.push_back(field.reverse() ? -1 : 1);
    }

    const size_t leaf_count = readers.size();
    auto queue = PriorityQueue<LeafAndDocId, LeafAndDocIdCompare>::create(
            leaf_count, LeafAndDocIdCompare(fields.size(), &reverse_muls));
    if (!queue.has_value()) {
        return std::unexpected(std::move(queue.error()));
    }

    std::vector<PackedLongValues::Builder> builders;
    builders.reserve(leaf_count);
    for (size_t i = 0; i < leaf_count; ++i) {
        auto max_doc = readers[i]->max_doc();
        if (!max_doc.has_value()) {
            return std::unexpected(std::move(max_doc.error()));
        }
        auto live_docs = readers[i]->live_docs();
        if (!live_docs.has_value()) {
            return std::unexpected(std::move(live_docs.error()));
        }
        LeafAndDocId cursor;
        cursor.reader_index = i;
        cursor.live_docs = live_docs.value();
        cursor.max_doc = max_doc.value();
        cursor.values_as_comparable_longs.assign(fields.size(), 0);
        cursor.doc_id = 0;
        if (cursor.max_doc > 0) {
            if (auto refreshed = refresh_values(cursor, providers); !refreshed.has_value()) {
                return std::unexpected(std::move(refreshed.error()));
            }
        }
        auto added = queue.value().add(std::move(cursor));
        if (!added.has_value()) {
            return std::unexpected(std::move(added.error()));
        }
        builders.emplace_back(PackedLongValues::DEFAULT_PAGE_SIZE, 64, 0, true);
    }

    int32_t mapped_doc_id = 0;
    size_t last_reader_index = 0;
    bool is_sorted = true;

    while (queue.value().size() > 0) {
        LeafAndDocId *top = queue.value().top_mut();
        if (top == nullptr) {
            return err(LuceneError::illegal_state("no top available"));
        }

        if (last_reader_index > top->reader_index) {
            // Documents of an earlier segment come after a later one: the merge
            // really does have to reorder.
            is_sorted = false;
        }
        last_reader_index = top->reader_index;

        if (auto added = builders[top->reader_index].add(mapped_doc_id); !added.has_value()) {
            return std::unexpected(std::move(added.error()));
        }

        auto live = is_live(*top);
        if (!live.has_value()) {
            return std::unexpected(std::move(live.error()));
        }
        if (live.value()) {
            mapped_doc_id += 1;
        }
        top->doc_id += 1;

        if (top->doc_id < top->max_doc) {
            if (auto refreshed = refresh_values(*top, providers); !refreshed.has_value()) {
                return std::unexpected(std::move(refreshed.error()));
            }
            auto updated = queue.value().update_top();
            if (!updated.has_value()) {
                return std::unexpected(std::move(updated.error()));
            }
        } else {
            auto popped = queue.value().pop();
            if (!popped.has_value()) {
                return std::unexpected(std::move(popped.error()));
            }
        }
    }

    if (is_sorted) {
        return std::optional<std::vector<std::shared_ptr<MergeStateDocMap>>>{};
    }

    std::vector<std::shared_ptr<MergeStateDocMap>> doc_maps;
    doc_maps.reserve(leaf_count);
    for (size_t i = 0; i < leaf_count; ++i) {
        auto remapped = builders[i].build();
        if (!remapped.has_value()) {
            return std::unexpected(std::move(remapped.error()));
        }
        auto live_docs = readers[i]->live_docs();
        if (!live_docs.has_value()) {
            return std::unexpected(std::move(live_docs.error()));
        }
        // The map does not outlive the reader, so a non-owning pointer is enough.
        std::shared_ptr<const Bits> live(live_docs.value(), [](const Bits *) {});
        doc_maps.push_back(MergeStateDocMap::new_sorted(
                std::move(live), std::make_shared<const PackedLongValues>(std::move(remapped.value()))));
    }
    return std::optional<std::vector<std::shared_ptr<MergeStateDocMap>>>{std::move(doc_maps)};
}

} // namespace lucene
