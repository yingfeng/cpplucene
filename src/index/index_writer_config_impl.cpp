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
import :lucene101_codec;
import :merge_policy;
import :merge_scheduler;
import :tiered_merge_policy;
import :similarity;
import :types;
import :version;

import std;

namespace lucene {

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
    merge_scheduler_ = SerialMergeScheduler::create();
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

} // namespace lucene
