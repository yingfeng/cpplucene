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

export module lucene_core:score_doc;

import :types;

import std;

namespace lucene {

/// One hit of a [`TopDocs`](lucene_core:top_docs).
///
/// Port of `ScoreDoc` (`core/search/score_doc.rs`).
///
/// **Deviation:** the `ScoreDocLike` trait that rlucene uses to put `ScoreDoc`
/// and `TopFieldScoreDoc` in one `TopDocs` is not ported; the port has a single
/// `TopDocs` over `ScoreDoc` until sorting lands.
export struct ScoreDoc {
    /// The score of this document for the query.
    float score{0.0F};

    /// The document number; see `StoredFields::document`.
    int32_t doc{0};

    /// Only set by the top-docs merge functions, `-1` otherwise.
    int32_t shard_index{-1};

    /// A hit with the default shard index.
    [[nodiscard]] static ScoreDoc create(int32_t doc, float score) { return ScoreDoc{score, doc, -1}; }

    /// A hit with an explicit shard index.
    [[nodiscard]] static ScoreDoc with_shard_index(int32_t doc, float score, int32_t shard_index) {
        return ScoreDoc{score, doc, shard_index};
    }

    /// `doc=<doc> score=<score> shardIndex=<index>`, for diagnostics.
    [[nodiscard]] std::string to_string() const {
        return "doc=" + std::to_string(doc) + " score=" + std::format("{}", score) +
               " shardIndex=" + std::to_string(shard_index);
    }

    friend bool operator==(const ScoreDoc &, const ScoreDoc &) = default;
};

} // namespace lucene
