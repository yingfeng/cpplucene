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

module lucene_core:pending_deletes.impl;

import :pending_deletes;

import std;

import :bits;
import :directory;
import :error;
import :fixed_bit_set;
import :io_context;
import :live_docs_format;
import :segment_commit_info;
import :segment_reader;
import :types;

namespace lucene {

Result<std::unique_ptr<PendingDeletes>> PendingDeletes::create(const LiveDocsFormat &format, const Directory &dir,
                                                              const SegmentCommitInfo &info,
                                                              const IOContext &context) {
    std::optional<FixedBitSet> live_docs;
    if (info.has_deletions()) {
        auto read = format.read_live_docs(dir, info, context);
        if (!read.has_value()) {
            return std::unexpected(std::move(read.error()));
        }
        live_docs = std::move(read.value());
    }
    // A segment without deletions starts out initialized: it may receive deletes
    // before anyone opens a reader on it (e.g. right after a merge).
    return std::unique_ptr<PendingDeletes>(new PendingDeletes(std::string(info.info.name), std::move(live_docs),
                                                              true, static_cast<int32_t>(info.max_doc())));
}

Result<std::unique_ptr<PendingDeletes>> PendingDeletes::from_reader(const SegmentReader &reader,
                                                                   const SegmentCommitInfo &info) {
    auto bits = reader.live_docs();
    if (!bits.has_value()) {
        return std::unexpected(std::move(bits.error()));
    }
    std::optional<FixedBitSet> live_docs;
    if (bits.value() != nullptr) {
        const auto *fixed = dynamic_cast<const FixedBitSet *>(bits.value());
        if (fixed == nullptr) {
            return err(LuceneError::illegal_state("the reader's live docs are not a fixed bit set"));
        }
        live_docs = *fixed;
    }
    auto pending = std::unique_ptr<PendingDeletes>(new PendingDeletes(
            std::string(info.info.name), std::move(live_docs), true, static_cast<int32_t>(info.max_doc())));
    auto num_deleted = reader.num_deleted_docs();
    if (!num_deleted.has_value()) {
        return std::unexpected(std::move(num_deleted.error()));
    }
    pending->pending_delete_count_ =
            static_cast<int32_t>(num_deleted.value()) - static_cast<int32_t>(info.del_count);
    return pending;
}

Result<std::unique_ptr<PendingDeletes>> PendingDeletes::with(std::string segment_name,
                                                             std::optional<FixedBitSet> live_docs, bool initialized,
                                                             int32_t max_doc) {
    if (max_doc < 0) {
        return err(LuceneError::illegal_argument("max_doc must not be negative"));
    }
    return std::unique_ptr<PendingDeletes>(
            new PendingDeletes(std::move(segment_name), std::move(live_docs), initialized, max_doc));
}

Result<FixedBitSet *> PendingDeletes::mutable_bits() {
    if (!live_docs_initialized_) {
        return err(LuceneError::illegal_state("cannot delete: the live docs are not initialized"));
    }
    if (!live_docs_.has_value()) {
        FixedBitSet bits(static_cast<size_t>(max_doc_));
        if (auto set = bits.set_with_range(0, static_cast<size_t>(max_doc_)); !set.has_value()) {
            return std::unexpected(std::move(set.error()));
        }
        live_docs_ = std::move(bits);
    }
    return &live_docs_.value();
}

Result<bool> PendingDeletes::delete_doc(int32_t doc_id) {
    auto bits = mutable_bits();
    if (!bits.has_value()) {
        return std::unexpected(std::move(bits.error()));
    }
    FixedBitSet &live = *bits.value();
    if (doc_id < 0 || static_cast<size_t>(doc_id) >= live.length()) {
        return err(LuceneError::illegal_argument(
                std::format("document {} is out of bounds for segment {} with max_doc {}", doc_id, segment_name_,
                            max_doc_)));
    }
    const size_t index = static_cast<size_t>(doc_id);
    const bool did_delete = live.get_bit(index);
    if (did_delete) {
        live.clear(index);
        ++pending_delete_count_;
    }
    return did_delete;
}

void PendingDeletes::delete_all() {
    if (!live_docs_initialized_) {
        return;
    }
    if (!live_docs_.has_value()) {
        FixedBitSet bits(static_cast<size_t>(max_doc_));
        // Everything is live by default; deleting all of it is a no-op on the bits.
        live_docs_ = std::move(bits);
    }
    live_docs_->clear_all();
    pending_delete_count_ = max_doc_;
}

Result<bool> PendingDeletes::is_deleted(int32_t doc_id) const {
    if (!live_docs_.has_value()) {
        return false;
    }
    if (doc_id < 0 || static_cast<size_t>(doc_id) >= live_docs_->length()) {
        return err(LuceneError::illegal_argument(std::format("document {} is out of bounds", doc_id)));
    }
    return !live_docs_->get_bit(static_cast<size_t>(doc_id));
}

VoidResult PendingDeletes::on_new_reader(const SegmentReader &reader) {
    if (live_docs_initialized_) {
        return {};
    }
    auto bits = reader.live_docs();
    if (!bits.has_value()) {
        return std::unexpected(std::move(bits.error()));
    }
    if (bits.value() != nullptr) {
        const auto *fixed = dynamic_cast<const FixedBitSet *>(bits.value());
        if (fixed == nullptr) {
            return err(LuceneError::illegal_state("the reader's live docs are not a fixed bit set"));
        }
        live_docs_ = *fixed;
    }
    live_docs_initialized_ = true;
    return {};
}

Result<bool> PendingDeletes::write_live_docs(const LiveDocsFormat &format, Directory &dir,
                                             const SegmentCommitInfo &info, int32_t new_del_count,
                                             const IOContext &context) {
    if (pending_delete_count_ == 0) {
        return false;
    }
    if (!live_docs_.has_value()) {
        return err(LuceneError::illegal_state("the live docs have to be initialized before they are written"));
    }
    if (live_docs_->length() != static_cast<size_t>(max_doc_) ||
        max_doc_ != static_cast<int32_t>(info.max_doc())) {
        return err(LuceneError::illegal_state(
                std::format("the live docs of segment {} do not match its document count", segment_name_)));
    }
    if (auto written = format.write_live_docs(live_docs_.value(), dir, info, new_del_count, context);
        !written.has_value()) {
        return std::unexpected(std::move(written.error()));
    }
    return true;
}

} // namespace lucene
