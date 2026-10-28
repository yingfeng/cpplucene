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

module lucene_core:index_writer_config.impl;

import :index_writer_config;

import :analyzer;
import :bm25_similarity;
import :codec;
import :error;
import :flush_policy;
import :index_deletion_policy;
import :lucene101_codec;
import :merge_policy;
import :concurrent_merge_scheduler;
import :merge_scheduler;
import :tiered_merge_policy;
import :similarity;
import :types;
import :version;

import std;

namespace lucene {

size_t default_indexing_buffers() noexcept {
    const size_t cores = static_cast<size_t>(std::max(1U, std::thread::hardware_concurrency()));
    return std::clamp<size_t>(cores, 2, 8);
}

IndexWriterConfig::IndexWriterConfig(Analyzer &analyzer)
        : analyzer_(&analyzer),
          similarity_([] {
              auto created = BM25Similarity::default_similarity();
              if (!created.has_value()) {
                  throw std::runtime_error("cannot create the default similarity: " +
                                           created.error().to_string());
              }
              return std::shared_ptr<const Similarity>(
                      std::make_shared<const BM25Similarity>(std::move(created.value())));
          }()),
          index_created_version_major_(LUCENE_CURRENT_VERSION.major) {
    auto codec = Lucene101Codec::create();
    if (!codec.has_value()) {
        throw std::runtime_error("cannot create the default codec: " + codec.error().to_string());
    }
    codec_ = std::shared_ptr<const Codec>(std::move(codec.value()));
    merge_policy_ = std::shared_ptr<MergePolicy>(TieredMergePolicy::create());
    // Merges run on background threads, like the reference implementation's
    // default: half the cores, between one and four workers.
    const int32_t cores = static_cast<int32_t>(std::max(1U, std::thread::hardware_concurrency()));
    merge_scheduler_ = ConcurrentMergeScheduler::create(std::clamp(cores / 2, 1, 4));
    index_deletion_policy_ = std::make_shared<KeepOnlyLastCommitDeletionPolicy>();
}

void IndexWriterConfig::set_index_deletion_policy(std::shared_ptr<IndexDeletionPolicy> policy) {
    index_deletion_policy_ = policy == nullptr ? std::make_shared<KeepOnlyLastCommitDeletionPolicy>() : std::move(policy);
}

void IndexWriterConfig::set_codec(std::shared_ptr<const Codec> codec) {
    if (codec == nullptr) {
        return;
    }
    codec_ = std::move(codec);
}

void IndexWriterConfig::set_similarity(std::shared_ptr<const Similarity> similarity) {
    if (similarity == nullptr) {
        return;
    }
    similarity_ = std::move(similarity);
}

void IndexWriterConfig::set_flush_policy(std::shared_ptr<const FlushPolicy> flush_policy) noexcept {
    flush_policy_ = flush_policy == nullptr ? FlushByRamOrCountsPolicy::create() : std::move(flush_policy);
}

void IndexWriterConfig::set_merge_policy(std::shared_ptr<MergePolicy> merge_policy) {
    if (merge_policy == nullptr) {
        return;
    }
    merge_policy_ = std::move(merge_policy);
}

void IndexWriterConfig::set_merge_scheduler(std::shared_ptr<MergeScheduler> merge_scheduler) {
    if (merge_scheduler == nullptr) {
        return;
    }
    merge_scheduler_ = std::move(merge_scheduler);
}

int64_t IndexWriterConfig::ram_buffer_size_bytes() const noexcept {
    // A negative or absurd value would disable the limit, which is what a caller
    // asking for it gets; everything else is megabytes of 1024 * 1024 bytes.
    if (!(ram_buffer_size_mb_ > 0.0)) {
        return std::numeric_limits<int64_t>::max();
    }
    const double bytes = ram_buffer_size_mb_ * 1024.0 * 1024.0;
    return bytes >= static_cast<double>(std::numeric_limits<int64_t>::max())
                   ? std::numeric_limits<int64_t>::max()
                   : static_cast<int64_t>(bytes);
}


void IndexWriterConfig::set_event_listener(std::shared_ptr<IndexWriterEventListener> listener) {
    event_listener_ = listener == nullptr ? std::make_shared<NoOpIndexWriterEventListener>() : std::move(listener);
}

void IndexWriterConfig::set_merged_segment_warmer(std::shared_ptr<IndexReaderWarmer> warmer) {
    merged_segment_warmer_ = std::move(warmer);
}

} // namespace lucene
