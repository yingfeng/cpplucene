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

module lucene_core:frozen_buffered_updates.impl;

import :frozen_buffered_updates;

import std;

import :buffered_updates;
import :bytes_ref;
import :error;
import :field_updates_buffer;
import :prefix_coded_terms;
import :query;
import :types;

namespace lucene {

Result<std::shared_ptr<FrozenBufferedUpdates>> FrozenBufferedUpdates::freeze(
        std::unique_ptr<BufferedUpdates> updates, std::optional<std::string> private_segment) {
    if (updates == nullptr) {
        return err(LuceneError::illegal_argument("cannot freeze null buffered updates"));
    }
    if (private_segment.has_value() && !updates->delete_terms().is_empty()) {
        return err(LuceneError::illegal_argument("a packet private to a segment may only hold delete queries"));
    }

    // Compress the terms; the values are collected in the same (sorted) order so
    // `term_limits` lines up with the packet's term iteration.
    PrefixCodedTerms::Builder builder;
    std::vector<int32_t> term_limits;
    term_limits.reserve(static_cast<size_t>(updates->delete_terms().size()));
    auto collected = updates->delete_terms().for_each_ordered(
            [&](std::string_view field, const BytesRef &bytes, int32_t limit) -> VoidResult {
                if (auto added = builder.add(field, bytes); !added.has_value()) {
                    return added;
                }
                term_limits.push_back(limit);
                return {};
            });
    if (!collected.has_value()) {
        return std::unexpected(std::move(collected.error()));
    }

    const int32_t bytes_used = static_cast<int32_t>(std::min<int64_t>(
            updates->ram_bytes_used(), std::numeric_limits<int32_t>::max()));
    const int32_t field_updates_count = updates->num_field_updates();

    std::vector<std::shared_ptr<const Query>> delete_queries;
    std::vector<int32_t> delete_query_limits;
    for (auto &[query, limit] : updates->take_delete_queries()) {
        delete_queries.push_back(std::move(query));
        delete_query_limits.push_back(limit);
    }

    return std::shared_ptr<FrozenBufferedUpdates>(new FrozenBufferedUpdates(
            builder.build(), std::move(term_limits), std::move(delete_queries), std::move(delete_query_limits),
            updates->take_field_updates(), field_updates_count, bytes_used, std::move(private_segment)));
}

std::string FrozenBufferedUpdates::to_string() const {
    std::string out = std::format("delGen={} terms={} queries={} fieldUpdates={} bytesUsed={}", del_gen_,
                                  delete_terms_.size(), delete_queries_.size(), field_updates_count_, bytes_used_);
    if (applied_) {
        out += " applied";
    }
    if (private_segment_.has_value()) {
        out += std::format(" privateSegment={}", private_segment_.value());
    }
    return out;
}

} // namespace lucene
