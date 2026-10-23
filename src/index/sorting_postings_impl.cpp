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

module lucene_core:sorting_postings.impl;

import :sorting_postings;

import std;

import :error;
import :index_sorter;
import :postings_enum;
import :types;

namespace lucene {

namespace {

[[nodiscard]] bool wants_positions(int32_t flags) {
    return feature_requested(flags, POSITIONS);
}

[[nodiscard]] bool wants_offsets(int32_t flags) {
    return feature_requested(flags, OFFSETS);
}

[[nodiscard]] bool wants_payloads(int32_t flags) {
    return feature_requested(flags, PAYLOADS);
}

} // namespace

SortingPostingsEnum::SortingPostingsEnum(std::vector<DocEntry> docs, std::vector<int32_t> positions,
                                         std::vector<int32_t> starts, std::vector<int32_t> ends,
                                         std::vector<std::vector<uint8_t>> payloads, bool has_offsets, bool has_payloads)
        : docs_(std::move(docs)),
          positions_(std::move(positions)),
          starts_(std::move(starts)),
          ends_(std::move(ends)),
          payloads_(std::move(payloads)),
          has_offsets_(has_offsets),
          has_payloads_(has_payloads) {}

Result<std::unique_ptr<SortingPostingsEnum>> SortingPostingsEnum::create(std::unique_ptr<PostingsEnum> delegate,
                                                                        const IndexSortDocMap &doc_map,
                                                                        int32_t flags) {
    if (delegate == nullptr) {
        return err(LuceneError::illegal_argument("the postings must not be null"));
    }

    const bool with_positions = wants_positions(flags);
    const bool with_offsets = wants_offsets(flags);
    const bool with_payloads = wants_payloads(flags);

    std::vector<DocEntry> docs;
    std::vector<int32_t> positions;
    std::vector<int32_t> starts;
    std::vector<int32_t> ends;
    std::vector<std::vector<uint8_t>> payloads;

    while (true) {
        auto old_doc = delegate->next_doc();
        if (!old_doc.has_value()) {
            return std::unexpected(std::move(old_doc.error()));
        }
        if (old_doc.value() == NO_MORE_DOCS) {
            break;
        }
        auto new_doc = doc_map.old_to_new(old_doc.value());
        if (!new_doc.has_value()) {
            return std::unexpected(std::move(new_doc.error()));
        }
        if (new_doc.value() < 0) {
            // The document was deleted before the sort.
            continue;
        }

        auto freq = delegate->freq();
        if (!freq.has_value()) {
            return std::unexpected(std::move(freq.error()));
        }

        DocEntry entry;
        entry.doc = new_doc.value();
        entry.freq = freq.value();
        entry.positions_start = positions.size();

        for (int32_t i = 0; i < freq.value(); ++i) {
            if (!with_positions) {
                // Without positions the per-position data is still one entry long
                // per term occurrence, so keep the arrays aligned.
                positions.push_back(-1);
                starts.push_back(-1);
                ends.push_back(-1);
                payloads.emplace_back();
                continue;
            }
            auto position = delegate->next_position();
            if (!position.has_value()) {
                return std::unexpected(std::move(position.error()));
            }
            positions.push_back(position.value());

            if (with_offsets) {
                auto start = delegate->start_offset();
                if (!start.has_value()) {
                    return std::unexpected(std::move(start.error()));
                }
                auto end = delegate->end_offset();
                if (!end.has_value()) {
                    return std::unexpected(std::move(end.error()));
                }
                starts.push_back(start.value());
                ends.push_back(end.value());
            } else {
                starts.push_back(-1);
                ends.push_back(-1);
            }

            if (with_payloads) {
                auto payload = delegate->get_payload();
                if (!payload.has_value()) {
                    return std::unexpected(std::move(payload.error()));
                }
                if (payload.value().has_value()) {
                    const auto bytes = payload.value().value();
                    payloads.emplace_back(bytes.begin(), bytes.end());
                } else {
                    payloads.emplace_back();
                }
            } else {
                payloads.emplace_back();
            }
        }
        docs.push_back(entry);
    }

    // Replay the postings in the sorted order of their documents.
    std::ranges::stable_sort(docs, [](const DocEntry &left, const DocEntry &right) { return left.doc < right.doc; });
    return std::unique_ptr<SortingPostingsEnum>(new SortingPostingsEnum(
            std::move(docs), std::move(positions), std::move(starts), std::move(ends), std::move(payloads),
            with_offsets, with_payloads));
}

const SortingPostingsEnum::DocEntry &SortingPostingsEnum::current() const {
    static const DocEntry kEmpty;
    if (doc_ < 0 || position_ == 0) {
        return kEmpty;
    }
    return docs_[position_ - 1];
}

Result<int32_t> SortingPostingsEnum::next_doc() {
    if (position_ < docs_.size()) {
        doc_ = docs_[position_].doc;
        position_ += 1;
        position_index_ = 0;
    } else {
        doc_ = NO_MORE_DOCS;
    }
    return doc_;
}

Result<int32_t> SortingPostingsEnum::advance(int32_t target) {
    auto begin = docs_.begin() + static_cast<std::ptrdiff_t>(position_);
    auto found = std::lower_bound(begin, docs_.end(), target,
                                 [](const DocEntry &entry, int32_t value) { return entry.doc < value; });
    if (found == docs_.end()) {
        position_ = docs_.size();
        doc_ = NO_MORE_DOCS;
        return doc_;
    }
    position_ = static_cast<size_t>(found - docs_.begin()) + 1;
    doc_ = found->doc;
    position_index_ = 0;
    return doc_;
}

Result<int64_t> SortingPostingsEnum::cost() const {
    return static_cast<int64_t>(docs_.size());
}

Result<int32_t> SortingPostingsEnum::freq() {
    if (doc_ < 0 || position_ == 0) {
        return err(LuceneError::illegal_state("the postings are not positioned on a document"));
    }
    return current().freq;
}

Result<int32_t> SortingPostingsEnum::next_position() {
    if (doc_ < 0 || position_ == 0) {
        return err(LuceneError::illegal_state("the postings are not positioned on a document"));
    }
    const DocEntry &entry = current();
    if (position_index_ >= static_cast<size_t>(entry.freq)) {
        return err(LuceneError::illegal_state("no more positions for this document"));
    }
    return positions_[entry.positions_start + position_index_++];
}

Result<int32_t> SortingPostingsEnum::start_offset() const {
    if (doc_ < 0 || position_ == 0) {
        return err(LuceneError::illegal_state("the postings are not positioned on a document"));
    }
    if (!has_offsets_) {
        return -1;
    }
    const DocEntry &entry = current();
    if (position_index_ == 0) {
        return err(LuceneError::illegal_state("no position was read yet"));
    }
    return starts_[entry.positions_start + position_index_ - 1];
}

Result<int32_t> SortingPostingsEnum::end_offset() const {
    if (doc_ < 0 || position_ == 0) {
        return err(LuceneError::illegal_state("the postings are not positioned on a document"));
    }
    if (!has_offsets_) {
        return -1;
    }
    const DocEntry &entry = current();
    if (position_index_ == 0) {
        return err(LuceneError::illegal_state("no position was read yet"));
    }
    return ends_[entry.positions_start + position_index_ - 1];
}

Result<std::optional<std::span<const uint8_t>>> SortingPostingsEnum::get_payload() const {
    if (doc_ < 0 || position_ == 0) {
        return err(LuceneError::illegal_state("the postings are not positioned on a document"));
    }
    if (!has_payloads_) {
        return std::optional<std::span<const uint8_t>>{};
    }
    const DocEntry &entry = current();
    if (position_index_ == 0) {
        return err(LuceneError::illegal_state("no position was read yet"));
    }
    const std::vector<uint8_t> &payload = payloads_[entry.positions_start + position_index_ - 1];
    if (payload.empty()) {
        return std::optional<std::span<const uint8_t>>{};
    }
    return std::optional<std::span<const uint8_t>>{std::span<const uint8_t>(payload)};
}

SortingDocsEnum::SortingDocsEnum(std::vector<int32_t> docs) : docs_(std::move(docs)) {}

Result<std::unique_ptr<SortingDocsEnum>> SortingDocsEnum::create(std::unique_ptr<PostingsEnum> delegate,
                                                                const IndexSortDocMap &doc_map) {
    if (delegate == nullptr) {
        return err(LuceneError::illegal_argument("the postings must not be null"));
    }

    std::vector<int32_t> docs;
    while (true) {
        auto old_doc = delegate->next_doc();
        if (!old_doc.has_value()) {
            return std::unexpected(std::move(old_doc.error()));
        }
        if (old_doc.value() == NO_MORE_DOCS) {
            break;
        }
        auto new_doc = doc_map.old_to_new(old_doc.value());
        if (!new_doc.has_value()) {
            return std::unexpected(std::move(new_doc.error()));
        }
        if (new_doc.value() < 0) {
            // The document was deleted before the sort.
            continue;
        }
        docs.push_back(new_doc.value());
    }

    std::ranges::sort(docs);
    return std::unique_ptr<SortingDocsEnum>(new SortingDocsEnum(std::move(docs)));
}

Result<int32_t> SortingDocsEnum::next_doc() {
    if (position_ < docs_.size()) {
        doc_ = docs_[position_];
        position_ += 1;
    } else {
        doc_ = NO_MORE_DOCS;
    }
    return doc_;
}

Result<int32_t> SortingDocsEnum::advance(int32_t target) {
    auto begin = docs_.begin() + static_cast<std::ptrdiff_t>(position_);
    auto found = std::lower_bound(begin, docs_.end(), target);
    if (found == docs_.end()) {
        position_ = docs_.size();
        doc_ = NO_MORE_DOCS;
        return doc_;
    }
    position_ = static_cast<size_t>(found - docs_.begin()) + 1;
    doc_ = *found;
    return doc_;
}

Result<int64_t> SortingDocsEnum::cost() const {
    return static_cast<int64_t>(docs_.size());
}

Result<int32_t> SortingDocsEnum::freq() {
    if (doc_ < 0) {
        return err(LuceneError::illegal_state("the postings are not positioned on a document"));
    }
    return 1;
}

Result<int32_t> SortingDocsEnum::next_position() {
    if (doc_ < 0) {
        return err(LuceneError::illegal_state("the postings are not positioned on a document"));
    }
    return -1;
}

Result<int32_t> SortingDocsEnum::start_offset() const {
    return -1;
}

Result<int32_t> SortingDocsEnum::end_offset() const {
    return -1;
}

Result<std::optional<std::span<const uint8_t>>> SortingDocsEnum::get_payload() const {
    return std::optional<std::span<const uint8_t>>{};
}

} // namespace lucene
